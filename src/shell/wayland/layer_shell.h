#pragma once

#include "ui/popup_parent.h"
#include "wayland/popup_parent_attach.h"
#include "wayland/wayland_extension.h"

#include <unordered_map>

class LayerSurface;
class WaylandConnection;
struct wl_surface;
struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;

// Binds zwlr_layer_shell_v1 and maps each LayerSurface's wl_surface back to it.
class LayerShell final : public WaylandExtension {
public:
  explicit LayerShell(WaylandConnection& connection);

  void onGlobal(wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version) override;
  void releaseGlobals() override;

  zwlr_layer_shell_v1* manager() const noexcept { return m_layerShell; }

  void registerSurface(const LayerSurface& layerSurface);
  void unregisterSurface(wl_surface* surface);
  zwlr_layer_surface_v1* layerSurfaceFor(wl_surface* surface) const noexcept;
  const LayerSurface* layerSurfaceOwnerFor(wl_surface* surface) const noexcept;

  // Parents a popup on layerSurface through zwlr_layer_surface_v1.get_popup; empty for nullptr.
  static PopupParentAttach popupAttach(zwlr_layer_surface_v1* layerSurface);
  // Switches the layer surface registered for surface to OnDemand keyboard interactivity while a
  // grabbing popup is open, and back to None afterwards if that layer surface still exists.
  PopupKeyboardGrab popupKeyboardGrab(wl_surface* surface);

private:
  zwlr_layer_shell_v1* m_layerShell = nullptr;
  std::unordered_map<wl_surface*, const LayerSurface*> m_surfaces;
};
