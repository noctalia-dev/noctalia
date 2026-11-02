#pragma once

#include <cstdint>

struct wl_output;
struct wl_surface;
struct xdg_surface;
struct zwlr_layer_surface_v1;

struct PopupAnchorRect {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t width = 1;
  std::int32_t height = 1;
};

struct XdgPopupParent {
  xdg_surface* xdgSurface = nullptr;
  wl_surface* wlSurface = nullptr;
  wl_output* output = nullptr;
  std::uint32_t serial = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

struct PopupSurfaceParent {
  zwlr_layer_surface_v1* layerSurface = nullptr;
  xdg_surface* xdgSurface = nullptr;
  wl_output* output = nullptr;
  // Parent surface. When set alongside a layerSurface whose keyboard
  // interactivity is None (e.g. a bar), the popup owner flips it to OnDemand
  // while open so the grabbing popup inherits keyboard focus.
  wl_surface* wlSurface = nullptr;
};
