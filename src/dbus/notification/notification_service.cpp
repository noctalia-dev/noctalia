#include "notification_service.h"

#include "core/log.h"
#include "dbus/session_bus.h"
#include "i18n/i18n.h"
#include "notification/notification_manager.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cstdint>
#include <tuple>

namespace {
  constexpr Logger kLog("notification");
} // namespace

static const sdbus::ServiceName kBusName{"org.freedesktop.Notifications"};
static const sdbus::ObjectPath kObjectPath{"/org/freedesktop/Notifications"};
static constexpr auto kInterface = "org.freedesktop.Notifications";
static constexpr auto kAlreadyOwnerError = "System.Error.EALREADY";

namespace {
  void requestNotificationBusName(sdbus::IConnection& connection) {
    try {
      connection.requestName(kBusName);
    } catch (const sdbus::Error& e) {
      if (e.getName() == kAlreadyOwnerError) {
        kLog.debug("notification daemon bus name already owned by this connection; reusing");
        return;
      }
      throw;
    }
  }
} // namespace

NotificationService::NotificationService(SessionBus& bus, NotificationManager& manager)
    : m_bus(bus), m_manager(manager) {
  try {
    m_object = sdbus::createObject(m_bus.connection(), kObjectPath);

    m_object
        ->addVTable(
            sdbus::registerMethod("Notify")
                .withInputParamNames(
                    "app_name", "replaces_id", "app_icon", "summary", "body", "actions", "hints", "expire_timeout"
                )
                .withOutputParamNames("id")
                .implementedAs([this](
                                   const std::string& app_name, uint32_t replaces_id, const std::string& app_icon,
                                   const std::string& summary, const std::string& body,
                                   const std::vector<std::string>& actions,
                                   const std::map<std::string, sdbus::Variant>& hints, int32_t expire_timeout
                               ) {
                  return onNotify(app_name, replaces_id, app_icon, summary, body, actions, hints, expire_timeout);
                }),

            sdbus::registerMethod("GetCapabilities").withOutputParamNames("capabilities").implementedAs([this]() {
              return onGetCapabilities();
            }),

            sdbus::registerMethod("GetNotifications").withOutputParamNames("notifications").implementedAs([this]() {
              return onGetNotifications();
            }),

            sdbus::registerMethod("GetServerInformation")
                .withOutputParamNames("name", "vendor", "version", "spec_version")
                .implementedAs([this]() { return onGetServerInformation(); }),

            sdbus::registerMethod("CloseNotification").withInputParamNames("id").implementedAs([this](uint32_t id) {
              onCloseNotification(id);
            }),

            sdbus::registerMethod("InvokeAction")
                .withInputParamNames("id", "action_key")
                .implementedAs([this](uint32_t id, const std::string& actionKey) { onInvokeAction(id, actionKey); }),

            sdbus::registerSignal("NotificationClosed").withParameters<uint32_t, uint32_t>("id", "reason"),

            sdbus::registerSignal("ActionInvoked").withParameters<uint32_t, std::string>("id", "action_key")
        )
        .forInterface(kInterface);

    requestNotificationBusName(m_bus.connection());
    m_nameAcquired = true;
    m_manager.setActionInvokeCallback([this](uint32_t id, const std::string& actionKey) {
      emitActionInvoked(id, actionKey);
    });
    m_manager.setCloseCallback([this](uint32_t id, CloseReason reason) { emitClose(id, reason); });
  } catch (...) {
    m_manager.setCloseCallback(nullptr);
    m_manager.setActionInvokeCallback(nullptr);
    if (m_nameAcquired) {
      try {
        m_bus.connection().releaseName(kBusName);
      } catch (const sdbus::Error& e) {
        kLog.debug("notification daemon release after init failure failed: {}", e.what());
      }
      m_nameAcquired = false;
    }
    throw;
  }
}

NotificationService::~NotificationService() {
  m_manager.setCloseCallback(nullptr);
  m_manager.setActionInvokeCallback(nullptr);

  if (m_nameAcquired) {
    try {
      m_bus.connection().releaseName(kBusName);
    } catch (const sdbus::Error& e) {
      kLog.debug("notification daemon bus name release failed: {}", e.what());
    }
    m_nameAcquired = false;
  }

  if (m_object != nullptr) {
    try {
      m_object->unregister();
    } catch (const sdbus::Error& e) {
      kLog.debug("notification daemon object unregister failed: {}", e.what());
    }
  }
}

void NotificationService::processExpired() {
  const std::vector<uint32_t> ids = m_manager.expiredIds();
  for (const uint32_t id : ids) {
    (void)m_manager.close(id, CloseReason::Expired);
  }
}

static constexpr size_t kMaxStringLen = 1024;
namespace {

  std::vector<std::string> sanitizeActions(const std::vector<std::string>& actions) {
    std::vector<std::string> sanitized;
    sanitized.reserve(actions.size() - (actions.size() % 2));

    for (size_t i = 0; i + 1 < actions.size(); i += 2) {
      std::string actionKey = StringUtils::truncateUtf8(actions[i], kMaxStringLen);
      std::string label = StringUtils::truncateUtf8(actions[i + 1], kMaxStringLen);

      if (actionKey.empty()) {
        continue;
      }

      if (StringUtils::isBlank(label)) {
        label = i18n::tr("notifications.actions.fallback");
      }

      sanitized.push_back(std::move(actionKey));
      sanitized.push_back(std::move(label));
    }

    return sanitized;
  }

  using NotificationImageDataStruct = sdbus::Struct<
      std::int32_t, std::int32_t, std::int32_t, bool, std::int32_t, std::int32_t, std::vector<std::uint8_t>>;

  std::optional<NotificationImageData> decodeImageDataVariant(const sdbus::Variant& value) {
    try {
      const auto data = value.get<NotificationImageDataStruct>();
      NotificationImageData out;
      out.width = std::get<0>(data);
      out.height = std::get<1>(data);
      out.rowStride = std::get<2>(data);
      out.hasAlpha = std::get<3>(data);
      out.bitsPerSample = std::get<4>(data);
      out.channels = std::get<5>(data);
      out.data = std::get<6>(data);
      return out;
    } catch (const sdbus::Error&) {
    }

    try {
      const auto data = value.get<std::tuple<
          std::int32_t, std::int32_t, std::int32_t, bool, std::int32_t, std::int32_t, std::vector<std::uint8_t>>>();
      NotificationImageData out;
      out.width = std::get<0>(data);
      out.height = std::get<1>(data);
      out.rowStride = std::get<2>(data);
      out.hasAlpha = std::get<3>(data);
      out.bitsPerSample = std::get<4>(data);
      out.channels = std::get<5>(data);
      out.data = std::get<6>(data);
      return out;
    } catch (const sdbus::Error&) {
    }

    return std::nullopt;
  }

  std::optional<NotificationImageData>
  decodeImageHint(const std::map<std::string, sdbus::Variant>& hints, std::string* outSourceKey = nullptr) {
    for (const char* key : {"image-data", "image_data", "icon_data"}) {
      const auto it = hints.find(key);
      if (it == hints.end()) {
        continue;
      }

      auto decoded = decodeImageDataVariant(it->second);
      if (decoded.has_value()) {
        if (outSourceKey != nullptr) {
          *outSourceKey = key;
        }
        return decoded;
      }
    }

    return std::nullopt;
  }

} // namespace

uint32_t NotificationService::onNotify(
    const std::string& app_name, uint32_t replaces_id, const std::string& app_icon, const std::string& summary,
    const std::string& body, const std::vector<std::string>& actions,
    const std::map<std::string, sdbus::Variant>& hints, int32_t expire_timeout
) {
  // Sanitize scalar inputs
  const int32_t timeout = normalizeNotifyExpireTimeout(expire_timeout);
  const auto sanitizedActions = sanitizeActions(actions);

  // Urgency: default Normal, reject out-of-range byte values
  Urgency urgency = Urgency::Normal;
  if (auto it = hints.find("urgency"); it != hints.end()) {
    try {
      const uint8_t raw = it->second.get<uint8_t>();
      if (raw <= static_cast<uint8_t>(Urgency::Critical)) {
        urgency = static_cast<Urgency>(raw);
      }
    } catch (...) {
    }
  }

  std::optional<std::string> icon;
  if (!app_icon.empty()) {
    icon = StringUtils::truncateUtf8(app_icon, kMaxStringLen);
  }
  if (auto it = hints.find("image-path"); it != hints.end()) {
    try {
      icon = StringUtils::truncateUtf8(it->second.get<std::string>(), kMaxStringLen);
    } catch (...) {
    }
  }
  if (auto it = hints.find("image_path"); it != hints.end()) {
    try {
      icon = StringUtils::truncateUtf8(it->second.get<std::string>(), kMaxStringLen);
    } catch (...) {
    }
  }

  std::optional<std::string> category;
  if (auto it = hints.find("category"); it != hints.end()) {
    try {
      category = StringUtils::truncateUtf8(it->second.get<std::string>(), kMaxStringLen);
    } catch (...) {
    }
  }

  std::optional<std::string> desktopEntry;
  if (auto it = hints.find("desktop-entry"); it != hints.end()) {
    try {
      desktopEntry = StringUtils::truncateUtf8(it->second.get<std::string>(), kMaxStringLen);
    } catch (...) {
    }
  }

  std::optional<NotificationImageData> imageData = decodeImageHint(hints);

  return m_manager.addOrReplace(
      replaces_id, StringUtils::truncateUtf8(app_name, kMaxStringLen),
      StringUtils::sanitizeMarkup(StringUtils::truncateUtf8(summary, kMaxStringLen)),
      StringUtils::sanitizeMarkup(StringUtils::truncateUtf8(body, kMaxStringLen)), urgency, timeout,
      NotificationOrigin::External, sanitizedActions, icon, imageData, category, desktopEntry
  );
}

std::vector<std::string> NotificationService::onGetCapabilities() { return {"body", "actions", "inline-reply"}; }

std::vector<std::map<std::string, sdbus::Variant>> NotificationService::onGetNotifications() {
  std::vector<std::map<std::string, sdbus::Variant>> result;
  for (const auto& n : m_manager.all()) {
    std::map<std::string, sdbus::Variant> notif;
    notif["id"] = sdbus::Variant(n.id);
    notif["app_name"] = sdbus::Variant(n.appName);
    notif["summary"] = sdbus::Variant(n.summary);
    notif["body"] = sdbus::Variant(n.body);
    notif["timeout"] = sdbus::Variant(n.timeout);
    notif["urgency"] = sdbus::Variant(static_cast<uint8_t>(n.urgency));
    notif["actions"] = sdbus::Variant(n.actions);
    notif["icon"] = sdbus::Variant(n.icon.value_or(""));
    notif["category"] = sdbus::Variant(n.category.value_or(""));
    notif["desktop_entry"] = sdbus::Variant(n.desktopEntry.value_or(""));
    result.push_back(notif);
  }
  return result;
}

void NotificationService::onCloseNotification(uint32_t id) {
  if (!m_manager.close(id, CloseReason::ClosedByCall)) {
    throw sdbus::Error(
        sdbus::Error::Name{"org.freedesktop.Notifications.Error.NotFound"}, "notification id was not found"
    );
  }
}

void NotificationService::emitClose(uint32_t id, CloseReason reason) {
  if (m_object == nullptr) {
    return;
  }
  try {
    m_object->emitSignal("NotificationClosed").onInterface(kInterface).withArguments(id, static_cast<uint32_t>(reason));
    m_bus.connection().processPendingEvent();
  } catch (const sdbus::Error& e) {
    kLog.debug("notification #{}: NotificationClosed emit failed: {}", id, e.what());
  }
}

void NotificationService::onInvokeAction(uint32_t id, const std::string& actionKey) {
  const std::string sanitizedKey = StringUtils::truncateUtf8(actionKey, kMaxStringLen);
  if (sanitizedKey.empty()) {
    throw sdbus::Error(
        sdbus::Error::Name{"org.freedesktop.Notifications.Error.InvalidAction"}, "action_key must not be empty"
    );
  }

  if (!m_manager.invokeAction(id, sanitizedKey, false)) {
    throw sdbus::Error(
        sdbus::Error::Name{"org.freedesktop.Notifications.Error.InvalidAction"},
        "action_key is not available for this notification"
    );
  }

  kLog.debug("notification action #{} key='{}'", id, sanitizedKey);
}

void NotificationService::emitActionInvoked(uint32_t id, const std::string& actionKey) {
  if (actionKey == "inline-reply") {
    kLog.warn("notification #{}: ActionInvoked with bare inline-reply (missing reply text)", id);
  } else if (actionKey.starts_with("inline-reply::")) {
    kLog.debug("notification #{}: inline-reply action invoked ({} bytes)", id, actionKey.size());
  } else {
    kLog.debug("notification #{}: action '{}'", id, actionKey);
  }
  if (m_object == nullptr) {
    return;
  }
  try {
    m_object->emitSignal("ActionInvoked").onInterface(kInterface).withArguments(id, actionKey);
    m_bus.connection().processPendingEvent();
  } catch (const sdbus::Error& e) {
    kLog.debug("notification #{}: ActionInvoked emit failed key='{}': {}", id, actionKey, e.what());
  }
}

std::tuple<std::string, std::string, std::string, std::string> NotificationService::onGetServerInformation() {
  return {"noctalia", "noctalia-dev", NOCTALIA_VERSION, "1.2"};
}
