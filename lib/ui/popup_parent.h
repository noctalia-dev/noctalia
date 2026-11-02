#pragma once

#include "wayland/popup_parent_attach.h"

#include <cstdint>
#include <functional>

struct wl_output;
struct wl_surface;
struct xdg_surface;

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

// Lets a parent that normally refuses keyboard focus (a bar) accept it while a grabbing popup is
// open, so the popup receives keys. Called before the popup maps. Returns the function that
// restores the parent after the popup closes, or an empty function when there is nothing to
// restore; the restore must tolerate the parent having been destroyed meanwhile.
using PopupKeyboardGrab = std::function<std::function<void()>()>;

// Exactly one of attach and xdgSurface names the parent.
struct PopupSurfaceParent {
  PopupParentAttach attach;
  xdg_surface* xdgSurface = nullptr;
  wl_output* output = nullptr;
  // Optional; parents that already accept keyboard focus leave it empty.
  PopupKeyboardGrab keyboardGrab;
};
