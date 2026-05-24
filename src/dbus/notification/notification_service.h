#pragma once

#include "notification/notification_manager.h"

#include <map>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <tuple>
#include <vector>

class SessionBus;

class NotificationService {
public:
  NotificationService(SessionBus& bus, NotificationManager& manager);
  ~NotificationService();

  // Close expired notifications and emit D-Bus signals for them.
  void processExpired();

private:
  SessionBus& m_bus;
  NotificationManager& m_manager;
  std::unique_ptr<sdbus::IObject> m_object;
  bool m_nameAcquired = false;

  // D-Bus method handlers
  uint32_t onNotify(
      const std::string& app_name, uint32_t replaces_id, const std::string& app_icon, const std::string& summary,
      const std::string& body, const std::vector<std::string>& actions,
      const std::map<std::string, sdbus::Variant>& hints, int32_t expire_timeout
  );

  void onCloseNotification(uint32_t id);
  void emitClose(uint32_t id, CloseReason reason);

  void onInvokeAction(uint32_t id, const std::string& actionKey);
  void emitActionInvoked(uint32_t id, const std::string& actionKey);

  std::vector<std::map<std::string, sdbus::Variant>> onGetNotifications();

  std::vector<std::string> onGetCapabilities();

  std::tuple<std::string, std::string, std::string, std::string> onGetServerInformation();
};
