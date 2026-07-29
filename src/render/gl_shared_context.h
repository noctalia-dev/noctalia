#pragma once

#include <EGL/egl.h>
#include <string_view>
#include <vector>

struct wl_display;

// Owns the EGLDisplay, the chosen EGLConfig, and a root surfaceless EGLContext
// that is the share parent for every other EGLContext in the shell
// (RenderContext, WallpaperRenderer instances, ...). By routing all contexts
// through this share group, GL textures uploaded in any of them are usable
// from every other context in the group — which lets LockSurface reuse
// wallpaper textures resident in VRAM without re-decoding.
class GlSharedContext {
public:
  GlSharedContext() = default;
  ~GlSharedContext();

  GlSharedContext(const GlSharedContext&) = delete;
  GlSharedContext& operator=(const GlSharedContext&) = delete;

  void initialize(wl_display* display, bool createSharedContext = true);
  void cleanup();
  void recreateRootContext();

  [[nodiscard]] EGLDisplay display() const noexcept { return m_display; }
  [[nodiscard]] EGLConfig config() const noexcept { return m_config; }
  [[nodiscard]] EGLContext rootContext() const noexcept { return m_rootContext; }
  [[nodiscard]] bool hasSharedContext() const noexcept { return m_rootContext != EGL_NO_CONTEXT; }
  [[nodiscard]] bool resetNotificationEnabled() const noexcept { return m_resetNotificationEnabled; }
  [[nodiscard]] bool videoMemoryPurgeNotificationEnabled() const noexcept {
    return m_videoMemoryPurgeNotificationEnabled;
  }

  [[nodiscard]] EGLContext createContext(EGLContext shareContext, std::string_view label);

  // Bind the root context surfacelessly. No-op (returns true) when shared context is disabled.
  // Returns false if eglMakeCurrent failed (e.g. context lost on resume); callers skip GPU work.
  bool makeCurrentSurfaceless() const;

private:
  void buildContextAttributes();
  [[nodiscard]] EGLContext createContextWithCurrentAttributes(EGLContext shareContext) const;
  void usePlainContextAttributes() noexcept;

  EGLDisplay m_display = EGL_NO_DISPLAY;
  EGLConfig m_config = nullptr;
  EGLContext m_rootContext = EGL_NO_CONTEXT;
  std::vector<EGLint> m_contextAttributes;
  bool m_contextAttributesRobust = false;
  bool m_resetNotificationEnabled = false;
  bool m_videoMemoryPurgeNotificationEnabled = false;
  bool m_sharedContextEnabled = false;
};
