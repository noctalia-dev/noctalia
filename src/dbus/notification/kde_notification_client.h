#pragma once

#include "dbus/notification/notification_dbus_host.h"

#include <cstdint>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>

class NotificationManager;
class SessionBus;

class KdeNotificationClient final : public NotificationDBusHost {
public:
  KdeNotificationClient(SessionBus& bus, NotificationManager& manager);
  ~KdeNotificationClient() override;

  void processExpired() override;
  [[nodiscard]] bool isHealthy() const override;

private:
  void onWatcherNotify(
      uint32_t id, const std::string& appName, uint32_t replacesId, const std::string& appIcon,
      const std::string& summary, const std::string& body, const std::vector<std::string>& actions,
      const std::map<std::string, sdbus::Variant>& hints, int32_t timeout
  );
  void onWatcherCloseNotification(uint32_t id);
  void proxyCloseNotification(uint32_t id);
  void proxyInvokeAction(uint32_t id, const std::string& actionKey);
  void unregisterWatcher();

  SessionBus& m_bus;
  NotificationManager& m_manager;
  std::unique_ptr<sdbus::IObject> m_watcherObject;
  std::unique_ptr<sdbus::IProxy> m_plasmaProxy;
  bool m_active = false;
  bool m_suppressPlasmaClose = false;
  uint32_t m_inhibitCookie = 0;
};
