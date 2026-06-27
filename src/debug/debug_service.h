#pragma once

#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string>

class SessionBus;
class NotificationManager;

class DebugService {
public:
  DebugService(SessionBus& bus, NotificationManager& notifications);

private:
  uint32_t onEmitInternalNotification(
      const std::string& app_name, const std::string& summary, const std::string& body, uint8_t urgency, int32_t timeout
  );
  bool onSetVerboseLogs(bool enabled);
  bool onGetVerboseLogs() const;

  NotificationManager& m_notifications;
  std::unique_ptr<sdbus::IObject> m_object;
  bool m_verboseLogs{false};
};
