// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux/fl_subsurface_egl.h"

#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "flutter/shell/platform/linux/fl_compositor_opengl_shader.h"

// EXPERIMENT (hz): presentation-time feedback statistics.
#include <gdk/gdkwayland.h>
#include <algorithm>
#include <ctime>
#include <vector>
#include "flutter/shell/platform/linux/hz_presentation_time.h"

// EXPERIMENT (hz): with FLUTTER_LINUX_PRESENT_STATS=1, ask the compositor for
// wp_presentation feedback on every frame presented to the subsurface and
// print what actually reached the display every 240 presented frames:
//   shown/s     frames the compositor reports as presented, per second
//   discarded   frames committed but replaced before being shown
//   msc Δ       vblank-counter step between consecutive shown frames
//               (1 = a new frame on every refresh, 2 = one refresh repeated)
//   gap         time between consecutive shown frames
//   latency     eglSwapBuffers -> scanout
// Feedback objects live on a private event queue that is dispatched from the
// raster thread at each present, so no callbacks run on the GTK thread.
namespace {

struct HzPresentStats {
  struct wl_display* display = nullptr;
  struct wl_event_queue* queue = nullptr;
  struct wp_presentation* presentation = nullptr;
  uint32_t clock_id = CLOCK_MONOTONIC;

  gint64 window_start_us = 0;
  int shown = 0;
  int discarded = 0;
  int msc_step[4] = {0, 0, 0, 0};  // 1, 2, 3, 4+
  uint32_t flags_all = ~0u;
  uint32_t flags_any = 0;
  uint32_t refresh_ns = 0;
  bool have_last = false;
  uint64_t last_msc = 0;
  int64_t last_ns = 0;
  std::vector<double> gaps_ms;
  std::vector<double> latency_ms;
};

HzPresentStats* hz_present_stats() {
  static HzPresentStats* stats = nullptr;
  static bool initialized = false;
  if (initialized) {
    return stats;
  }
  initialized = true;
  if (g_strcmp0(g_getenv("FLUTTER_LINUX_PRESENT_STATS"), "1") != 0) {
    return nullptr;
  }
  GdkDisplay* gdk_display = gdk_display_get_default();
  if (gdk_display == nullptr || !GDK_IS_WAYLAND_DISPLAY(gdk_display)) {
    return nullptr;
  }
  stats = new HzPresentStats();
  stats->display = gdk_wayland_display_get_wl_display(gdk_display);
  return stats;
}

int64_t hz_now_ns(clockid_t clock) {
  struct timespec ts;
  clock_gettime(clock, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

double hz_pct(std::vector<double>& v, double q) {
  if (v.empty()) {
    return 0;
  }
  std::sort(v.begin(), v.end());
  return v[static_cast<size_t>((v.size() - 1) * q)];
}

void hz_maybe_report(HzPresentStats* s) {
  if (s->shown < 60) {
    return;
  }
  gint64 now = g_get_monotonic_time();
  double secs = (now - s->window_start_us) / 1e6;
  g_message(
      "[hz] present: shown/s %.1f | discarded %d | msc Δ 1:%d 2:%d 3:%d 4+:%d | "
      "gap p10 %.2f / p50 %.2f / p90 %.2f ms | latency p50 %.2f / p90 %.2f ms "
      "| refresh %.3f ms | flags all 0x%x any 0x%x",
      s->shown / secs, s->discarded, s->msc_step[0], s->msc_step[1],
      s->msc_step[2], s->msc_step[3], hz_pct(s->gaps_ms, .1),
      hz_pct(s->gaps_ms, .5), hz_pct(s->gaps_ms, .9),
      hz_pct(s->latency_ms, .5), hz_pct(s->latency_ms, .9),
      s->refresh_ns / 1e6, s->flags_all, s->flags_any);
  s->window_start_us = now;
  s->shown = 0;
  s->discarded = 0;
  for (int& c : s->msc_step) {
    c = 0;
  }
  s->flags_all = ~0u;
  s->flags_any = 0;
  s->gaps_ms.clear();
  s->latency_ms.clear();
}

struct HzFeedback {
  HzPresentStats* stats;
  int64_t swap_ns;
};

void hz_feedback_sync_output(void*,
                             struct wp_presentation_feedback*,
                             struct wl_output*) {}

void hz_feedback_presented(void* data,
                           struct wp_presentation_feedback* feedback,
                           uint32_t tv_sec_hi,
                           uint32_t tv_sec_lo,
                           uint32_t tv_nsec,
                           uint32_t refresh,
                           uint32_t seq_hi,
                           uint32_t seq_lo,
                           uint32_t flags) {
  HzFeedback* fb = static_cast<HzFeedback*>(data);
  HzPresentStats* s = fb->stats;
  int64_t t = static_cast<int64_t>((static_cast<uint64_t>(tv_sec_hi) << 32) |
                                   tv_sec_lo) *
                  1000000000 +
              tv_nsec;
  uint64_t msc = (static_cast<uint64_t>(seq_hi) << 32) | seq_lo;
  if (s->have_last) {
    s->gaps_ms.push_back((t - s->last_ns) / 1e6);
    uint64_t step = msc - s->last_msc;
    s->msc_step[step >= 4 ? 3 : (step == 0 ? 0 : step - 1)]++;
  }
  s->have_last = true;
  s->last_ns = t;
  s->last_msc = msc;
  s->latency_ms.push_back((t - fb->swap_ns) / 1e6);
  s->refresh_ns = refresh;
  s->flags_all &= flags;
  s->flags_any |= flags;
  s->shown++;
  wp_presentation_feedback_destroy(feedback);
  delete fb;
  hz_maybe_report(s);
}

void hz_feedback_discarded(void* data,
                           struct wp_presentation_feedback* feedback) {
  HzFeedback* fb = static_cast<HzFeedback*>(data);
  fb->stats->discarded++;
  wp_presentation_feedback_destroy(feedback);
  delete fb;
}

const struct wp_presentation_feedback_listener hz_feedback_listener = {
    hz_feedback_sync_output, hz_feedback_presented, hz_feedback_discarded};

void hz_presentation_clock_id(void* data,
                              struct wp_presentation*,
                              uint32_t clk_id) {
  static_cast<HzPresentStats*>(data)->clock_id = clk_id;
}

const struct wp_presentation_listener hz_presentation_listener = {
    hz_presentation_clock_id};

void hz_registry_global(void* data,
                        struct wl_registry* registry,
                        uint32_t name,
                        const char* interface,
                        uint32_t version) {
  HzPresentStats* s = static_cast<HzPresentStats*>(data);
  if (g_strcmp0(interface, "wp_presentation") == 0) {
    s->presentation = static_cast<struct wp_presentation*>(wl_registry_bind(
        registry, name, &hz_wp_presentation_interface, MIN(version, 1u)));
    wp_presentation_add_listener(s->presentation, &hz_presentation_listener,
                                 s);
  }
}

void hz_registry_global_remove(void*, struct wl_registry*, uint32_t) {}

const struct wl_registry_listener hz_registry_listener = {
    hz_registry_global, hz_registry_global_remove};

// Called on the raster thread before each eglSwapBuffers.
void hz_present_begin(struct wl_surface* surface) {
  HzPresentStats* s = hz_present_stats();
  if (s == nullptr) {
    return;
  }
  if (s->queue == nullptr) {
    s->queue = wl_display_create_queue(s->display);
    struct wl_display* wrapper =
        static_cast<struct wl_display*>(wl_proxy_create_wrapper(s->display));
    wl_proxy_set_queue(reinterpret_cast<struct wl_proxy*>(wrapper), s->queue);
    struct wl_registry* registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    wl_registry_add_listener(registry, &hz_registry_listener, s);
    wl_display_roundtrip_queue(s->display, s->queue);  // globals
    wl_display_roundtrip_queue(s->display, s->queue);  // clock_id
    g_message("[hz] presentation feedback %s (clock_id %u)",
              s->presentation != nullptr ? "enabled" : "unavailable",
              s->clock_id);
    s->window_start_us = g_get_monotonic_time();
  }
  if (s->presentation == nullptr) {
    return;
  }
  wl_display_dispatch_queue_pending(s->display, s->queue);

  // Feedback must be requested before the commit made by eglSwapBuffers.
  struct wl_surface* surface_wrapper =
      static_cast<struct wl_surface*>(wl_proxy_create_wrapper(surface));
  wl_proxy_set_queue(reinterpret_cast<struct wl_proxy*>(surface_wrapper),
                     s->queue);
  struct wp_presentation_feedback* feedback =
      wp_presentation_feedback(s->presentation, surface_wrapper);
  wl_proxy_wrapper_destroy(surface_wrapper);
  wp_presentation_feedback_add_listener(
      feedback, &hz_feedback_listener,
      new HzFeedback{s, hz_now_ns(static_cast<clockid_t>(s->clock_id))});
}

}  // namespace

struct _FlSubsurfaceEGL {
  GObject parent_instance;

  // OpenGL manager providing the shared EGL display and context. The display is
  // owned by the engine and must not be terminated here.
  FlOpenGLManager* opengl_manager;

  // Subsurface frames are presented to. A reference is held so its Wayland
  // surface outlives the EGL surface created from it.
  FlSubsurface* subsurface;

  // Native Wayland window backing the EGL surface.
  struct wl_egl_window* egl_window;

  // EGL context used to blit engine frames. Shares resources with the engine's
  // render context.
  EGLContext egl_context;

  // EGL surface that draws onto egl_window.
  EGLSurface egl_surface;

  // TRUE if glBlitFramebuffer can be used to copy the engine frame to the
  // window surface. When FALSE the frame is drawn with [shader] instead.
  gboolean can_blit;

  // Framebuffer used to read the engine's frame texture. Created lazily on the
  // first present and reused for subsequent frames. Only used when [can_blit].
  GLuint read_framebuffer;

  // Shader used to draw the engine frame when glBlitFramebuffer is unavailable.
  // NULL when [can_blit].
  FlCompositorOpenGLShader* shader;

  // The buffer scale set on the subsurface. Buffer sizes have to be an integer
  // multiple of this.
  gint scale;

  // The size egl_window has been resized to. The buffer for the next frame is
  // acquired when the current one is swapped, so this is the size that buffer
  // will be, not the size of the buffer being drawn into now.
  size_t window_width;
  size_t window_height;

  // The size of the buffer the next frame is drawn into. eglQuerySurface()
  // reports the size of the buffer that was last swapped rather than this one,
  // so the size is tracked here instead.
  size_t buffer_width;
  size_t buffer_height;
};

G_DEFINE_TYPE(FlSubsurfaceEGL, fl_subsurface_egl, G_TYPE_OBJECT)

// Rounds a size in device pixels up to a whole number of logical pixels.
// Wayland requires buffer sizes to be an integer multiple of the buffer scale,
// and a view that hasn't been allocated yet can produce a frame that isn't,
// e.g. a single pixel frame on a display with a scale of two.
static size_t round_to_scale(size_t size, gint scale) {
  return ((size + scale - 1) / scale) * scale;
}

// Gets the EGL display the engine renders to. The subsurface shares this
// display so its context can access the engine's frame texture directly.
static EGLDisplay get_display(FlSubsurfaceEGL* self) {
  return fl_opengl_manager_get_display(self->opengl_manager);
}

// Sets up the EGL context and window surface for the subsurface. If this
// fails a warning is printed and the object is left without a usable context;
// subsequent operations will fail with the usual EGL/OpenGL errors.
static void setup(FlSubsurfaceEGL* self,
                  size_t width,
                  size_t height,
                  gint scale) {
  // The native window is created in device pixels below.
  self->scale = scale;
  self->window_width = width * scale;
  self->window_height = height * scale;
  self->buffer_width = self->window_width;
  self->buffer_height = self->window_height;

  // Share the engine's EGL display and render context so the engine's frame
  // texture can be accessed directly, without using EGLImage.
  EGLDisplay egl_display = get_display(self);
  if (egl_display == EGL_NO_DISPLAY) {
    g_warning("Failed to get EGL display for subsurface");
    return;
  }

  static const EGLint config_attributes[] = {EGL_SURFACE_TYPE,
                                             EGL_WINDOW_BIT,
                                             EGL_RENDERABLE_TYPE,
                                             EGL_OPENGL_ES2_BIT,
                                             EGL_RED_SIZE,
                                             8,
                                             EGL_GREEN_SIZE,
                                             8,
                                             EGL_BLUE_SIZE,
                                             8,
                                             EGL_ALPHA_SIZE,
                                             8,
                                             EGL_NONE};
  EGLConfig egl_config;
  EGLint num_config;
  if (!eglChooseConfig(egl_display, config_attributes, &egl_config, 1,
                       &num_config) ||
      num_config == 0) {
    g_warning("Failed to choose EGL config for subsurface");
    return;
  }

  eglBindAPI(EGL_OPENGL_ES_API);

  static const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
                                              EGL_NONE};
  EGLContext share_context =
      fl_opengl_manager_get_context(self->opengl_manager);
  self->egl_context = eglCreateContext(egl_display, egl_config, share_context,
                                       context_attributes);
  if (self->egl_context == EGL_NO_CONTEXT) {
    g_warning("Failed to create EGL context for subsurface");
    return;
  }

  struct wl_surface* surface = fl_subsurface_get_surface(self->subsurface);
  self->egl_window =
      wl_egl_window_create(surface, width * scale, height * scale);
  if (self->egl_window == nullptr) {
    g_warning("Failed to create wl_egl_window for subsurface");
    return;
  }

  self->egl_surface = eglCreateWindowSurface(
      egl_display, egl_config,
      reinterpret_cast<EGLNativeWindowType>(self->egl_window), nullptr);
  if (self->egl_surface == EGL_NO_SURFACE) {
    g_warning("Failed to create EGL window surface for subsurface");
    return;
  }

  wl_surface_set_buffer_scale(surface, scale);

  eglMakeCurrent(egl_display, self->egl_surface, self->egl_surface,
                 self->egl_context);
  eglSwapInterval(egl_display, 0);

  // Determine whether this context can use glBlitFramebuffer to copy the engine
  // frame to the window surface; if not, fall back to compositing the frame
  // with a shader, matching the OpenGL renderer.
  self->can_blit = fl_opengl_manager_can_blit(self->opengl_manager);
  eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

  if (!self->can_blit) {
    // The shader is created in the engine's context, which the subsurface
    // context shares, so its program and buffers are usable when the subsurface
    // context is current.
    self->shader = fl_compositor_opengl_shader_new(self->opengl_manager);
    fl_opengl_manager_clear_current(self->opengl_manager);
  }
}

static void fl_subsurface_egl_dispose(GObject* object) {
  FlSubsurfaceEGL* self = FL_SUBSURFACE_EGL(object);

  // The EGL display is owned by the engine (same Wayland display), so only the
  // surface and context created here are destroyed; the display must not be
  // terminated.
  if (self->opengl_manager != nullptr) {
    EGLDisplay egl_display = get_display(self);
    if (self->egl_context != EGL_NO_CONTEXT) {
      if (self->read_framebuffer != 0) {
        eglMakeCurrent(egl_display, self->egl_surface, self->egl_surface,
                       self->egl_context);
        glDeleteFramebuffers(1, &self->read_framebuffer);
        self->read_framebuffer = 0;
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
      }
    }
    if (self->egl_surface != EGL_NO_SURFACE) {
      eglDestroySurface(egl_display, self->egl_surface);
      self->egl_surface = EGL_NO_SURFACE;
    }
    if (self->egl_context != EGL_NO_CONTEXT) {
      eglDestroyContext(egl_display, self->egl_context);
      self->egl_context = EGL_NO_CONTEXT;
    }
  }
  if (self->egl_window != nullptr) {
    wl_egl_window_destroy(self->egl_window);
    self->egl_window = nullptr;
  }
  g_clear_object(&self->shader);
  g_clear_object(&self->subsurface);
  g_clear_object(&self->opengl_manager);

  G_OBJECT_CLASS(fl_subsurface_egl_parent_class)->dispose(object);
}

static void fl_subsurface_egl_class_init(FlSubsurfaceEGLClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = fl_subsurface_egl_dispose;
}

static void fl_subsurface_egl_init(FlSubsurfaceEGL* self) {
  self->egl_context = EGL_NO_CONTEXT;
  self->egl_surface = EGL_NO_SURFACE;
}

FlSubsurfaceEGL* fl_subsurface_egl_new(FlOpenGLManager* opengl_manager,
                                       FlSubsurface* subsurface,
                                       size_t width,
                                       size_t height,
                                       gint scale) {
  FlSubsurfaceEGL* self =
      FL_SUBSURFACE_EGL(g_object_new(fl_subsurface_egl_get_type(), nullptr));
  self->opengl_manager = FL_OPENGL_MANAGER(g_object_ref(opengl_manager));
  self->subsurface = FL_SUBSURFACE(g_object_ref(subsurface));

  setup(self, width, height, scale);

  return self;
}

void fl_subsurface_egl_resize(FlSubsurfaceEGL* self,
                              size_t width,
                              size_t height) {
  g_return_if_fail(FL_IS_SUBSURFACE_EGL(self));

  if (self->egl_window != nullptr) {
    width = round_to_scale(width, self->scale);
    height = round_to_scale(height, self->scale);
    wl_egl_window_resize(self->egl_window, width, height, 0, 0);
    self->window_width = width;
    self->window_height = height;
  }
}

void fl_subsurface_egl_present(FlSubsurfaceEGL* self,
                               GLuint texture_id,
                               size_t width,
                               size_t height,
                               FlGLFence* fence) {
  g_return_if_fail(FL_IS_SUBSURFACE_EGL(self));

  // Present the composited frame to the subsurface window surface using the
  // subsurface's own EGL context.
  EGLDisplay egl_display = get_display(self);
  eglMakeCurrent(egl_display, self->egl_surface, self->egl_surface,
                 self->egl_context);

  // Make this context wait for the frame to have finished rendering in the
  // engine's context before it reads the texture below. This context is the one
  // that reads the frame, so the waiting can be left to OpenGL and this thread
  // doesn't have to block.
  if (fence != nullptr) {
    fl_gl_fence_wait(fence);
  }

  size_t target_width = round_to_scale(width, self->scale);
  size_t target_height = round_to_scale(height, self->scale);

  if (self->egl_window != nullptr && (self->window_width != target_width ||
                                      self->window_height != target_height)) {
    wl_egl_window_resize(self->egl_window, target_width, target_height, 0, 0);
    self->window_width = target_width;
    self->window_height = target_height;
  }

  if (self->egl_window != nullptr && (self->buffer_width != target_width ||
                                      self->buffer_height != target_height)) {
    // The buffer a frame is drawn into is acquired when the previous frame is
    // swapped, so resizing the window doesn't reach the buffer this frame
    // would be drawn into. Swap that buffer away unused so this frame is drawn
    // into one of the size it was rendered at. A window sized to its content
    // only produces one frame at each size, so leaving this until the next
    // frame would leave it showing a buffer that was never painted.
    eglSwapBuffers(egl_display, self->egl_surface);
    self->buffer_width = self->window_width;
    self->buffer_height = self->window_height;
  }

  // The frame and the buffer are both window sized, so their dimensions fit in
  // a GLint and the arithmetic below can stay signed.
  GLint frame_width = static_cast<GLint>(width);
  GLint frame_height = static_cast<GLint>(height);

  // OpenGL puts the origin at the bottom left of the buffer but Wayland puts it
  // at the top left, so the frame has to be written at the top of the buffer
  // when the buffer is taller, which happens when the frame size was rounded up
  // to a whole logical pixel above.
  GLint dst_y0 = MAX(static_cast<GLint>(self->buffer_height) - frame_height, 0);

  // The subsurface context shares resources with the engine, so the engine's
  // frame texture can be read directly without using EGLImage.
  if (self->can_blit) {
    // Attach the frame texture to a persistent framebuffer and blit it to the
    // window surface. The framebuffer is created lazily on the first present
    // and reused for subsequent frames.
    if (self->read_framebuffer == 0) {
      glGenFramebuffers(1, &self->read_framebuffer);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, self->read_framebuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, texture_id, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, frame_width, frame_height, 0, dst_y0, frame_width,
                      dst_y0 + frame_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  } else {
    // glBlitFramebuffer is unavailable; draw the frame texture as a fullscreen
    // quad with the shader instead.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, dst_y0, frame_width, frame_height);
    fl_compositor_opengl_shader_use(self->shader);
    fl_compositor_opengl_shader_set_offset(self->shader, 0, 0);
    fl_compositor_opengl_shader_set_scale(self->shader, 1, 1);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glDrawArrays(GL_TRIANGLES, 0, 6);
  }
  hz_present_begin(fl_subsurface_get_surface(self->subsurface));  // EXPERIMENT
  eglSwapBuffers(egl_display, self->egl_surface);
  self->buffer_width = self->window_width;
  self->buffer_height = self->window_height;

  // Restore the engine's rendering context so the raster thread can continue
  // rendering after this present.
  fl_opengl_manager_make_current(self->opengl_manager);
}
