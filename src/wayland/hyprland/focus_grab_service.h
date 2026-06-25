#pragma once

#include <functional>
#include <memory>

struct hyprland_focus_grab_manager_v1;
struct hyprland_focus_grab_v1;
struct wl_surface;
class PopupGrabHost;

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
// that bind the protocol (Hyprland). Consumers should check available() before
// calling createGrab(); when unavailable, createGrab() returns nullptr and the
// caller should fall back to whatever non-Hyprland behavior makes sense (e.g.
// PanelClickShield).
class FocusGrabService {
public:
  FocusGrabService() = default;
  ~FocusGrabService() = default;

  FocusGrabService(const FocusGrabService&) = delete;
  FocusGrabService& operator=(const FocusGrabService&) = delete;

  // Pass nullptr when the protocol is not bound; available() will be false.
  void initialize(hyprland_focus_grab_manager_v1* manager);

  [[nodiscard]] bool available() const noexcept { return m_manager != nullptr; }

  // Returns nullptr when !available() or on protocol failure.
  [[nodiscard]] std::unique_ptr<FocusGrab> createGrab();

  // Currently-active popup grab host, or nullptr. PopupSurface consults this
  // on init to enroll itself in the host's whitelist (and to suppress the
  // conflicting xdg_popup_grab call). Owners install themselves while their
  // grab is up and clear the slot when it ends.
  void setPopupGrabHost(PopupGrabHost* host) noexcept { m_popupGrabHost = host; }
  [[nodiscard]] PopupGrabHost* popupGrabHost() const noexcept { return m_popupGrabHost; }

private:
  hyprland_focus_grab_manager_v1* m_manager = nullptr;
  PopupGrabHost* m_popupGrabHost = nullptr;
};
