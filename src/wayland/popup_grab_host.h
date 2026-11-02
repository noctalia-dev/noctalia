#pragma once

struct wl_surface;

// Implemented by the owner of a compositor focus grab that stands in for xdg_popup_grab (e.g. Hyprland's
// focus grab). While the grab is active the owner publishes itself with WaylandConnection::setPopupGrabHost();
// PopupSurface then enrolls its wl_surface with the host instead of calling xdg_popup_grab, so interacting with the
// popup does not count as a click outside the grab, and unregisters on destroy.
class PopupGrabHost {
public:
  virtual ~PopupGrabHost() = default;

  virtual void registerPopupSurface(wl_surface* surface) = 0;
  virtual void unregisterPopupSurface(wl_surface* surface) = 0;
};
