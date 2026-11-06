#pragma once

#include "wayland/wayland_extension.h"

#include <functional>

class WaylandConnection;
struct ext_idle_notification_v1;
struct ext_idle_notifier_v1;
struct ext_session_lock_manager_v1;
struct zwlr_gamma_control_manager_v1;
struct zwp_idle_inhibit_manager_v1;

// Managers for shell services: session lock, idle notify and inhibit, and gamma control.
class ShellGlobals final : public WaylandExtension {
public:
  using ChangeCallback = std::function<void()>;

  explicit ShellGlobals(WaylandConnection& connection);

  void onGlobal(wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version) override;
  void releaseGlobals() override;

  ext_session_lock_manager_v1* sessionLockManager() const noexcept { return m_sessionLockManager; }
  zwlr_gamma_control_manager_v1* gammaControlManager() const noexcept { return m_gammaControlManager; }
  zwp_idle_inhibit_manager_v1* idleInhibitManager() const noexcept { return m_idleInhibitManager; }
  bool hasIdleNotifier() const noexcept { return m_idleNotifier != nullptr; }

  /// Fired when both `ext_idle_notifier_v1` and `wl_seat` are bound (including late registry globals).
  void setIdleCapabilitiesReadyCallback(ChangeCallback callback);
  /// Inhibitor-aware idle notification (`get_idle_notification`); honors `zwp_idle_inhibitor_v1`.
  ext_idle_notification_v1* createIdleNotification(std::uint32_t timeoutMs) const;
  /// Input-only idle notification (`get_input_idle_notification`, notifier v2); ignores idle inhibitors.
  /// Returns null when the compositor's notifier is older than v2.
  ext_idle_notification_v1* createInputIdleNotification(std::uint32_t timeoutMs) const;

private:
  void notifyIdleCapabilitiesReady();

  WaylandConnection& m_connection;
  ext_session_lock_manager_v1* m_sessionLockManager = nullptr;
  ext_idle_notifier_v1* m_idleNotifier = nullptr;
  zwp_idle_inhibit_manager_v1* m_idleInhibitManager = nullptr;
  zwlr_gamma_control_manager_v1* m_gammaControlManager = nullptr;
  ChangeCallback m_idleCapabilitiesReadyCallback;
};
