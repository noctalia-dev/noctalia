#include "shell/wayland/shell_globals.h"

#include "ext-idle-notify-v1-client-protocol.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "wayland/wayland_connection.h"
#include "wlr-gamma-control-unstable-v1-client-protocol.h"

#include <algorithm>
#include <utility>

namespace {

  constexpr std::uint32_t kExtSessionLockManagerVersion = 1;
  constexpr std::uint32_t kExtIdleNotifierVersion = 2;
  constexpr std::uint32_t kIdleInhibitManagerVersion = 1;
  constexpr std::uint32_t kGammaControlManagerVersion = 1;

} // namespace

ShellGlobals::ShellGlobals(WaylandConnection& connection) : m_connection(connection) {}

void ShellGlobals::onGlobal(
    wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version
) {
  if (interface == wl_seat_interface.name) {
    notifyIdleCapabilitiesReady();
    return;
  }

  if (interface == ext_session_lock_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kExtSessionLockManagerVersion);
    m_sessionLockManager = static_cast<ext_session_lock_manager_v1*>(
        wl_registry_bind(registry, name, &ext_session_lock_manager_v1_interface, bindVersion)
    );
    return;
  }

  if (interface == ext_idle_notifier_v1_interface.name) {
    const auto bindVersion = std::min(version, kExtIdleNotifierVersion);
    m_idleNotifier = static_cast<ext_idle_notifier_v1*>(
        wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, bindVersion)
    );
    notifyIdleCapabilitiesReady();
    return;
  }

  if (interface == zwp_idle_inhibit_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kIdleInhibitManagerVersion);
    m_idleInhibitManager = static_cast<zwp_idle_inhibit_manager_v1*>(
        wl_registry_bind(registry, name, &zwp_idle_inhibit_manager_v1_interface, bindVersion)
    );
    return;
  }

  if (interface == zwlr_gamma_control_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kGammaControlManagerVersion);
    m_gammaControlManager = static_cast<zwlr_gamma_control_manager_v1*>(
        wl_registry_bind(registry, name, &zwlr_gamma_control_manager_v1_interface, bindVersion)
    );
  }
}

void ShellGlobals::releaseGlobals() {
  if (m_sessionLockManager != nullptr) {
    ext_session_lock_manager_v1_destroy(m_sessionLockManager);
    m_sessionLockManager = nullptr;
  }
  if (m_idleNotifier != nullptr) {
    ext_idle_notifier_v1_destroy(m_idleNotifier);
    m_idleNotifier = nullptr;
  }
  if (m_idleInhibitManager != nullptr) {
    zwp_idle_inhibit_manager_v1_destroy(m_idleInhibitManager);
    m_idleInhibitManager = nullptr;
  }
  if (m_gammaControlManager != nullptr) {
    zwlr_gamma_control_manager_v1_destroy(m_gammaControlManager);
    m_gammaControlManager = nullptr;
  }
}

void ShellGlobals::setIdleCapabilitiesReadyCallback(ChangeCallback callback) {
  m_idleCapabilitiesReadyCallback = std::move(callback);
  notifyIdleCapabilitiesReady();
}

void ShellGlobals::notifyIdleCapabilitiesReady() {
  if (m_idleNotifier == nullptr || m_connection.seat() == nullptr || !m_idleCapabilitiesReadyCallback) {
    return;
  }
  m_idleCapabilitiesReadyCallback();
}

ext_idle_notification_v1* ShellGlobals::createIdleNotification(std::uint32_t timeoutMs) const {
  wl_seat* const seat = m_connection.seat();
  if (m_idleNotifier == nullptr || seat == nullptr) {
    return nullptr;
  }

  return ext_idle_notifier_v1_get_idle_notification(m_idleNotifier, timeoutMs, seat);
}

ext_idle_notification_v1* ShellGlobals::createInputIdleNotification(std::uint32_t timeoutMs) const {
  wl_seat* const seat = m_connection.seat();
  if (m_idleNotifier == nullptr
      || seat == nullptr
      || ext_idle_notifier_v1_get_version(m_idleNotifier)
          < EXT_IDLE_NOTIFIER_V1_GET_INPUT_IDLE_NOTIFICATION_SINCE_VERSION) {
    return nullptr;
  }

  return ext_idle_notifier_v1_get_input_idle_notification(m_idleNotifier, timeoutMs, seat);
}
