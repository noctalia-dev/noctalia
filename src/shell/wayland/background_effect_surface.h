#pragma once

#include <vector>

class BackgroundEffectGlobals;
class WaylandConnection;
struct InputRect;
struct ext_background_effect_surface_v1;
struct wl_surface;

// Compositor blur behind a shell-owned wl_surface that is not a LayerSurface, such as an xdg popup.
// Destroy it before the wl_surface it was created for.
class BackgroundEffectSurface final {
public:
  BackgroundEffectSurface(WaylandConnection& connection, wl_surface* surface);
  ~BackgroundEffectSurface();

  BackgroundEffectSurface(const BackgroundEffectSurface&) = delete;
  BackgroundEffectSurface& operator=(const BackgroundEffectSurface&) = delete;

  // Blurs what lies behind rects (surface-local) from the surface's next commit, when the compositor supports blur.
  void setBlurRegion(const std::vector<InputRect>& rects);
  void clearBlurRegion();

private:
  WaylandConnection& m_connection;
  BackgroundEffectGlobals& m_backgroundEffects;
  wl_surface* m_surface = nullptr;
  ext_background_effect_surface_v1* m_effect = nullptr;
};
