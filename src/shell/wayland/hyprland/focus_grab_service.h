#pragma once

#include "wayland/wayland_extension.h"

#include <functional>
#include <memory>

class WaylandConnection;
struct hyprland_focus_grab_manager_v1;
struct hyprland_focus_grab_v1;
struct wl_surface;

// A single focus grab. While committed with at least one whitelisted surface,
// Hyprland routes pointer/keyboard events landing on a whitelisted surface to
// that surface as usual. Events outside the whitelist clear the grab and fire
// `cleared`. The grab does not auto-deactivate on `cleared`; the owner decides
// (typically: close the popup/panel that the grab was protecting).
//
// add_surface / remove_surface are batched; call commit() to apply pending
// changes. Destroying the FocusGrab destroys the protocol object and ends the
// grab.
class FocusGrab {
public:
  using ClearedCallback = std::function<void()>;

  ~FocusGrab();

  FocusGrab(const FocusGrab&) = delete;
  FocusGrab& operator=(const FocusGrab&) = delete;

  void addSurface(wl_surface* surface);
  void removeSurface(wl_surface* surface);
  void commit();

  void setOnCleared(ClearedCallback callback);

  // Public so the C-callback bridge can dispatch.
  static void handleCleared(void* data, hyprland_focus_grab_v1* grab);

private:
  friend class FocusGrabService;
  explicit FocusGrab(hyprland_focus_grab_v1* grab);

  hyprland_focus_grab_v1* m_grab = nullptr;
  ClearedCallback m_onCleared;
};

// Wrapper around hyprland_focus_grab_manager_v1. Available only on compositors
// that advertise the protocol (Hyprland). Consumers should check available() before
// calling createGrab(); when unavailable, createGrab() returns nullptr and the
// caller should fall back to whatever non-Hyprland behavior makes sense (e.g.
// PanelClickShield).
class FocusGrabService final : public WaylandExtension {
public:
  explicit FocusGrabService(WaylandConnection& connection);

  void onGlobal(wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version) override;
  void releaseGlobals() override;

  bool available() const noexcept { return m_manager != nullptr; }

  // Returns nullptr when !available() or on protocol failure.
  [[nodiscard]] std::unique_ptr<FocusGrab> createGrab();

private:
  hyprland_focus_grab_manager_v1* m_manager = nullptr;
};
