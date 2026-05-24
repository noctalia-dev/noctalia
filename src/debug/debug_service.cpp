#include "debug/debug_service.h"

#include "core/log.h"
#include "dbus/session_bus.h"

namespace {

  constexpr Logger kLog("debug");

  static const sdbus::ServiceName kDebugBusName{"dev.noctalia.Debug"};
  static const sdbus::ObjectPath kDebugObjectPath{"/dev/noctalia/Debug"};
  static constexpr auto kDebugInterface = "dev.noctalia.Debug";

  Urgency clamp_urgency(uint8_t urgency) {
    if (urgency > static_cast<uint8_t>(Urgency::Critical)) {
      return Urgency::Normal;
    }
    return static_cast<Urgency>(urgency);
  }

} // namespace

DebugService::DebugService(SessionBus& bus, NotificationManager& notifications) : m_notifications(notifications) {
  bus.connection().requestName(kDebugBusName);
  m_object = sdbus::createObject(bus.connection(), kDebugObjectPath);

  m_object
      ->addVTable(
          sdbus::registerMethod("EmitInternalNotification")
              .withInputParamNames("app_name", "summary", "body", "timeout", "urgency")
              .withOutputParamNames("id")
              .implementedAs([this](
                                 const std::string& app_name, const std::string& summary, const std::string& body,
                                 int32_t timeout, uint8_t urgency
                             ) { return onEmitInternalNotification(app_name, summary, body, urgency, timeout); }),

          sdbus::registerMethod("SetVerboseLogs")
              .withInputParamNames("enabled")
              .withOutputParamNames("success")
              .implementedAs([this](bool enabled) { return onSetVerboseLogs(enabled); }),

          sdbus::registerMethod("GetVerboseLogs").withOutputParamNames("enabled").implementedAs([this]() {
            return onGetVerboseLogs();
          })
      )
      .forInterface(kDebugInterface);
}

uint32_t DebugService::onEmitInternalNotification(
    const std::string& app_name, const std::string& summary, const std::string& body, uint8_t urgency, int32_t timeout
) {
  const uint32_t id = m_notifications.addInternal(app_name, summary, body, clamp_urgency(urgency), timeout);
  kLog.info("debug internal notification emitted id={} app=\"{}\"", id, app_name);
  return id;
}

bool DebugService::onSetVerboseLogs(bool enabled) {
  m_verboseLogs = enabled;
  setLogLevel(enabled ? LogLevel::Debug : LogLevel::Info);
  kLog.info("debug verbose logs {}", enabled ? "enabled" : "disabled");
  return true;
}

bool DebugService::onGetVerboseLogs() const { return m_verboseLogs; }
