#include "shell/wayland/layer_shell.h"

#include "shell/wayland/layer_surface.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <algorithm>

namespace {

  constexpr std::uint32_t kLayerShellVersion = 4;

} // namespace

LayerShell::LayerShell(WaylandConnection& /*connection*/) {}

void LayerShell::onGlobal(
    wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version
) {
  if (interface == zwlr_layer_shell_v1_interface.name) {
    const auto bindVersion = std::min(version, kLayerShellVersion);
    m_layerShell = static_cast<zwlr_layer_shell_v1*>(
        wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, bindVersion)
    );
  }
}

void LayerShell::releaseGlobals() {
  if (m_layerShell != nullptr) {
    zwlr_layer_shell_v1_destroy(m_layerShell);
    m_layerShell = nullptr;
  }
  m_surfaces.clear();
}

void LayerShell::registerSurface(const LayerSurface& layerSurface) {
  if (layerSurface.wlSurface() != nullptr && layerSurface.layerSurface() != nullptr) {
    m_surfaces[layerSurface.wlSurface()] = &layerSurface;
  }
}

void LayerShell::unregisterSurface(wl_surface* surface) {
  if (surface != nullptr) {
    m_surfaces.erase(surface);
  }
}

zwlr_layer_surface_v1* LayerShell::layerSurfaceFor(wl_surface* surface) const noexcept {
  const LayerSurface* owner = layerSurfaceOwnerFor(surface);
  return owner != nullptr ? owner->layerSurface() : nullptr;
}

const LayerSurface* LayerShell::layerSurfaceOwnerFor(wl_surface* surface) const noexcept {
  if (surface == nullptr) {
    return nullptr;
  }
  const auto it = m_surfaces.find(surface);
  return it != m_surfaces.end() ? it->second : nullptr;
}

PopupParentAttach LayerShell::popupAttach(zwlr_layer_surface_v1* layerSurface) {
  if (layerSurface == nullptr) {
    return {};
  }
  return [layerSurface](xdg_popup* popup) { zwlr_layer_surface_v1_get_popup(layerSurface, popup); };
}

PopupKeyboardGrab LayerShell::popupKeyboardGrab(wl_surface* surface) {
  return [this, surface] -> std::function<void()> {
    zwlr_layer_surface_v1* layerSurface = layerSurfaceFor(surface);
    if (layerSurface == nullptr) {
      return {};
    }
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        layerSurface, static_cast<std::uint32_t>(LayerShellKeyboard::OnDemand)
    );
    wl_surface_commit(surface);
    return [this, surface, layerSurface] {
      // A bar rebuild or output removal can destroy the parent while the popup is open, freeing
      // its proxies, so only touch it while it is still registered.
      if (layerSurfaceFor(surface) != layerSurface) {
        return;
      }
      zwlr_layer_surface_v1_set_keyboard_interactivity(
          layerSurface, static_cast<std::uint32_t>(LayerShellKeyboard::None)
      );
      wl_surface_commit(surface);
    };
  };
}
