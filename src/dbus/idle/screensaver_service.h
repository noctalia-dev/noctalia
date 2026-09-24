#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <vector>

class SystemBus;

/// D-Bus idle inhibits for apps that block the screensaver (Chrome, Firefox, Steam, portal/logind).
class ScreenSaverService {
public:
  using ChangeCallback = std::function<void(std::int64_t inhibitLocks)>;

  explicit ScreenSaverService(SystemBus* systemBus);
  ~ScreenSaverService();

  ScreenSaverService(const ScreenSaverService&) = delete;
  ScreenSaverService& operator=(const ScreenSaverService&) = delete;

  [[nodiscard]] bool active() const noexcept { return m_active || m_logindProxy != nullptr; }
  [[nodiscard]] bool hasScreenSaverBus() const noexcept { return m_active; }
  [[nodiscard]] std::int64_t inhibitLocks() const noexcept { return m_inhibitLocks; }
  [[nodiscard]] bool hasPendingEvents() const noexcept { return m_hasPendingEvents; }
  [[nodiscard]] sdbus::IConnection::PollData getPollData() const;
  void processPendingEvents();
  void setChangeCallback(ChangeCallback callback);

  /// Emits ActiveChanged on each registered org.freedesktop.ScreenSaver object.
  void emitActiveChanged(bool active);

private:
  struct InhibitCookie {
    std::uint32_t cookie = 0;
    std::string app;
    std::string reason;
    std::string ownerId;
  };

  void registerScreenSaver();
  void registerLogindIdleMonitor(SystemBus* systemBus);
  void applyLogindBlockInhibited(const std::string& inhibits);
  std::uint32_t onInhibit(std::string app, std::string reason, const char* sender);
  void onUninhibit(std::uint32_t cookie, const char* sender);
  void onInhibitDelta(std::int64_t delta);
  [[nodiscard]] std::size_t unregisterOwnerCookies(const std::string& ownerId);

  ChangeCallback m_changeCallback;
  std::unique_ptr<sdbus::IConnection> m_connection;
  std::unique_ptr<sdbus::IProxy> m_dbusProxy;
  std::unique_ptr<sdbus::IProxy> m_logindProxy;
  std::int64_t m_inhibitLocks = 0;
  std::uint32_t m_nextCookieId = 1337;
  bool m_active = false;
  bool m_logindIdleInhibited = false;
  bool m_hasPendingEvents = false;
  std::vector<InhibitCookie> m_cookies;
  std::vector<std::unique_ptr<sdbus::IObject>> m_objects;
};
