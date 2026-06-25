#pragma once

#include "dbus/notification/notification_dbus_host.h"
#include "notification/notification.h"

#include <map>
#include <memory>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <tuple>
#include <vector>

class SessionBus;
class NotificationManager;

namespace notification_dbus {

  inline constexpr const char* kFreedesktopNotificationsBusName = "org.freedesktop.Notifications";

  void acquireBusName(sdbus::IConnection& connection);
  [[nodiscard]] bool ownsBusName(sdbus::IConnection& connection);

  [[nodiscard]] Urgency notifyUrgencyFromHints(const std::map<std::string, sdbus::Variant>& hints);
  [[nodiscard]] bool notifyTransientFromHints(const std::map<std::string, sdbus::Variant>& hints);
  [[nodiscard]] std::vector<std::string> sanitizeNotifyActions(const std::vector<std::string>& actions);
  [[nodiscard]] std::optional<std::string> notifyIcon(
      const std::string& appName, const std::string& appIcon, const std::map<std::string, sdbus::Variant>& hints
  );
  [[nodiscard]] std::optional<NotificationImageData>
  notifyImageDataFromHints(const std::map<std::string, sdbus::Variant>& hints);
  [[nodiscard]] std::optional<std::string> notifyCategoryFromHints(const std::map<std::string, sdbus::Variant>& hints);
  [[nodiscard]] std::optional<std::string>
  notifyDesktopEntryFromHints(const std::map<std::string, sdbus::Variant>& hints);

  uint32_t ingestNotify(
      NotificationManager& manager, const std::string& app_name, uint32_t replaces_id, const std::string& app_icon,
      const std::string& summary, const std::string& body, const std::vector<std::string>& actions,
      const std::map<std::string, sdbus::Variant>& hints, int32_t expire_timeout
  );

} // namespace notification_dbus

class NotificationService final : public NotificationDBusHost {
public:
  NotificationService(SessionBus& bus, NotificationManager& manager);
  ~NotificationService() override;

  void processExpired() override;
  [[nodiscard]] bool isHealthy() const override;

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
