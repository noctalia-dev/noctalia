#include "shell/wayland/background_effect_surface.h"

#include "ext-background-effect-v1-client-protocol.h"
#include "shell/wayland/background_effect_globals.h"
#include "wayland/surface.h"
#include "wayland/wayland_connection.h"

#include <wayland-client-protocol.h>

BackgroundEffectSurface::BackgroundEffectSurface(WaylandConnection& connection, wl_surface* surface)
    : m_connection(connection), m_backgroundEffects(connection.extension<BackgroundEffectGlobals>()),
      m_surface(surface) {}

BackgroundEffectSurface::~BackgroundEffectSurface() { clearBlurRegion(); }

void BackgroundEffectSurface::setBlurRegion(const std::vector<InputRect>& rects) {
  if (rects.empty()) {
    clearBlurRegion();
    return;
  }
  if (m_effect == nullptr) {
    auto* manager = m_backgroundEffects.manager();
    if (m_surface == nullptr || manager == nullptr || !m_backgroundEffects.hasBlur()) {
      return;
    }
    m_effect = ext_background_effect_manager_v1_get_background_effect(manager, m_surface);
    if (m_effect == nullptr) {
      return;
    }
  }

  wl_region* region = wl_compositor_create_region(m_connection.compositor());
  if (region == nullptr) {
    return;
  }
  for (const auto& r : rects) {
    wl_region_add(region, r.x, r.y, r.width, r.height);
  }
  ext_background_effect_surface_v1_set_blur_region(m_effect, region);
  wl_region_destroy(region);
}

void BackgroundEffectSurface::clearBlurRegion() {
  if (m_effect == nullptr) {
    return;
  }
  ext_background_effect_surface_v1_destroy(m_effect);
  m_effect = nullptr;
}
