#include "notification_service.h"

#include "compositors/compositor_detect.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "i18n/i18n.h"
#include "net/uri.h"
#include "notification/notification_manager.h"
#include "render/core/image_decoder.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <cstdint>
#include <tuple>
#include <unistd.h>

namespace {
  constexpr Logger kLog("notification");
} // namespace

static const sdbus::ServiceName kBusName{notification_dbus::kFreedesktopNotificationsBusName};
static const sdbus::ObjectPath kObjectPath{"/org/freedesktop/Notifications"};
static constexpr auto kInterface = "org.freedesktop.Notifications";

namespace {

  const sdbus::ServiceName kDbusBusName{"org.freedesktop.DBus"};
  const sdbus::ObjectPath kDbusObjectPath{"/org/freedesktop/DBus"};
  constexpr auto kDbusInterface = "org.freedesktop.DBus";

  constexpr uint32_t kNameFlagAllowReplacement = 1U;
  constexpr uint32_t kNameFlagReplaceExisting = 2U;
  constexpr uint32_t kNameFlagDoNotQueue = 4U;

  constexpr uint32_t kNameReplyPrimaryOwner = 1U;
  constexpr uint32_t kNameReplyInQueue = 2U;
  constexpr uint32_t kNameReplyExists = 3U;
  constexpr uint32_t kNameReplyAlreadyOwner = 4U;

  [[nodiscard]] std::unique_ptr<sdbus::IProxy> dbusDaemonProxy(sdbus::IConnection& connection) {
    return sdbus::createProxy(connection, kDbusBusName, kDbusObjectPath);
  }

  [[nodiscard]] uint32_t notificationNameRequestFlags() {
    if (compositors::isKde()) {
      return kNameFlagAllowReplacement | kNameFlagReplaceExisting | kNameFlagDoNotQueue;
    }
    return kNameFlagAllowReplacement | kNameFlagDoNotQueue;
  }

} // namespace

namespace notification_dbus {

  void acquireBusName(sdbus::IConnection& connection) {
    auto proxy = dbusDaemonProxy(connection);

    const uint32_t flags = notificationNameRequestFlags();
    uint32_t reply = 0;
    proxy->callMethod("RequestName")
        .onInterface(kDbusInterface)
        .withArguments(std::string{kFreedesktopNotificationsBusName}, flags)
        .storeResultsTo(reply);

    if (reply == kNameReplyPrimaryOwner || reply == kNameReplyAlreadyOwner) {
      return;
    }

    const char* detail = reply == kNameReplyExists ? "org.freedesktop.Notifications is owned by another service"
        : reply == kNameReplyInQueue               ? "org.freedesktop.Notifications acquisition was queued"
                                                   : "unexpected RequestName reply";
    throw sdbus::Error(sdbus::Error::Name{"org.freedesktop.DBus.Error.AccessDenied"}, detail);
  }

  bool ownsBusName(sdbus::IConnection& connection) {
    try {
      auto proxy = dbusDaemonProxy(connection);
      std::string owner;
      proxy->callMethod("GetNameOwner")
          .onInterface(kDbusInterface)
          .withArguments(std::string{kFreedesktopNotificationsBusName})
          .storeResultsTo(owner);
      return owner == connection.getUniqueName();
    } catch (const sdbus::Error& e) {
      kLog.debug("notification GetNameOwner failed: {}", e.what());
      return false;
    }
  }

} // namespace notification_dbus

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

            sdbus::registerMethod("GetNotifications")
                .withOutputParamNames("active_notifications")
                .implementedAs([this]() { return onGetNotifications(); }),

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

    notification_dbus::acquireBusName(m_bus.connection());
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

bool NotificationService::isHealthy() const {
  if (!m_nameAcquired) {
    return false;
  }
  return notification_dbus::ownsBusName(m_bus.connection());
}

static constexpr size_t kMaxStringLen = 1024;

namespace notification_dbus {

  Urgency notifyUrgencyFromHints(const std::map<std::string, sdbus::Variant>& hints) {
    Urgency urgency = Urgency::Normal;
    if (auto it = hints.find("urgency"); it != hints.end()) {
      try {
        const auto raw = it->second.get<uint8_t>();
        if (raw <= static_cast<uint8_t>(Urgency::Critical)) {
          urgency = static_cast<Urgency>(raw);
        }
      } catch (...) {
      }
    }
    return urgency;
  }

  bool notifyTransientFromHints(const std::map<std::string, sdbus::Variant>& hints) {
    if (auto it = hints.find("transient"); it != hints.end()) {
      try {
        return it->second.get<bool>();
      } catch (...) {
      }
    }
    return false;
  }

  std::vector<std::string> sanitizeNotifyActions(const std::vector<std::string>& actions) {
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

  std::optional<std::string> notifyIcon(
      const std::string& /*appName*/, const std::string& appIcon, const std::map<std::string, sdbus::Variant>& hints
  ) {
    std::optional<std::string> icon;
    if (!appIcon.empty()) {
      icon = StringUtils::truncateUtf8(appIcon, kMaxStringLen);
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
    return icon;
  }

  std::optional<std::string> notifyCategoryFromHints(const std::map<std::string, sdbus::Variant>& hints) {
    if (auto it = hints.find("category"); it != hints.end()) {
      try {
        return StringUtils::truncateUtf8(it->second.get<std::string>(), kMaxStringLen);
      } catch (...) {
      }
    }
    return std::nullopt;
  }

  std::optional<std::string> notifyDesktopEntryFromHints(const std::map<std::string, sdbus::Variant>& hints) {
    if (auto it = hints.find("desktop-entry"); it != hints.end()) {
      try {
        return StringUtils::truncateUtf8(it->second.get<std::string>(), kMaxStringLen);
      } catch (...) {
      }
    }
    return std::nullopt;
  }

  namespace {

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

    std::optional<NotificationImageData> decodeImageDataFromImagePath(std::string_view imagePath) {
      // Many screenshot tools (e.g. HyprCap) provide a single temp file via "image-path" which gets
      // overwritten on every new capture. Snapshot it into `imageData` so history thumbnails stay
      // immutable.
      const std::string normalizedPath = uri::normalizeFileUrl(std::string(imagePath));
      if (normalizedPath.empty() || normalizedPath.front() != '/') {
        return std::nullopt;
      }
      if (access(normalizedPath.c_str(), R_OK) != 0) {
        return std::nullopt;
      }

      const auto bytes = FileUtils::readBinaryFile(normalizedPath);
      if (bytes.empty()) {
        return std::nullopt;
      }

      const auto decoded = decodeRasterImage(bytes.data(), bytes.size());
      if (!decoded) {
        return std::nullopt;
      }

      NotificationImageData out;
      out.width = decoded->width;
      out.height = decoded->height;
      out.rowStride = decoded->width * 4;
      out.hasAlpha = true;
      out.bitsPerSample = 8;
      out.channels = 4;
      out.data = decoded->pixels;
      return out;
    }

  } // namespace

  std::optional<NotificationImageData> notifyImageDataFromHints(const std::map<std::string, sdbus::Variant>& hints) {
    for (const char* key : {"image-data", "image_data", "icon_data"}) {
      const auto it = hints.find(key);
      if (it == hints.end()) {
        continue;
      }

      auto decoded = decodeImageDataVariant(it->second);
      if (decoded.has_value()) {
        return decoded;
      }
    }

    // Fallback: some notifiers only provide a path to a changing screenshot file (e.g. HyprCap).
    // If we can decode it immediately, persist pixels via `imageData`.
    for (const char* key : {"image-path", "image_path"}) {
      const auto it = hints.find(key);
      if (it == hints.end()) {
        continue;
      }
      try {
        const auto path = it->second.get<std::string>();
        if (path.empty()) {
          continue;
        }
        const std::string truncated = StringUtils::truncateUtf8(path, kMaxStringLen);
        if (truncated.empty()) {
          continue;
        }
        if (std::optional<NotificationImageData> decoded = decodeImageDataFromImagePath(truncated);
            decoded.has_value()) {
          return decoded;
        }
      } catch (...) {
      }
    }

    return std::nullopt;
  }

  uint32_t ingestNotify(
      NotificationManager& manager, const std::string& app_name, uint32_t replaces_id, const std::string& app_icon,
      const std::string& summary, const std::string& body, const std::vector<std::string>& actions,
      const std::map<std::string, sdbus::Variant>& hints, int32_t expire_timeout
  ) {
    const int32_t timeout = normalizeNotifyExpireTimeout(expire_timeout);
    const auto sanitizedActions = sanitizeNotifyActions(actions);

    return manager.addOrReplace(
        NotificationRequest{
            .replacesId = replaces_id,
            .appName = StringUtils::truncateUtf8(app_name, kMaxStringLen),
            .summary = StringUtils::sanitizeMarkup(StringUtils::truncateUtf8(summary, kMaxStringLen)),
            .body = StringUtils::sanitizeMarkup(StringUtils::truncateUtf8(body, kMaxStringLen)),
            .urgency = notifyUrgencyFromHints(hints),
            .timeout = timeout,
            .origin = NotificationOrigin::External,
            .transient = notifyTransientFromHints(hints),
            .actions = sanitizedActions,
            .icon = notifyIcon(app_name, app_icon, hints),
            .imageData = notifyImageDataFromHints(hints),
            .category = notifyCategoryFromHints(hints),
            .desktopEntry = notifyDesktopEntryFromHints(hints),
        }
    );
  }

} // namespace notification_dbus

uint32_t NotificationService::onNotify(
    const std::string& app_name, uint32_t replaces_id, const std::string& app_icon, const std::string& summary,
    const std::string& body, const std::vector<std::string>& actions,
    const std::map<std::string, sdbus::Variant>& hints, int32_t expire_timeout
) {
  return notification_dbus::ingestNotify(
      m_manager, app_name, replaces_id, app_icon, summary, body, actions, hints, expire_timeout
  );
}

std::vector<std::string> NotificationService::onGetCapabilities() {
  return {"actions", "body", "persistence", "inline-reply"};
}

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
    // Only queue the outgoing signal here. Do NOT pump the bus with
    // processPendingEvent(): emitClose runs inside NotificationManager::close()
    // (itself invoked from the session-bus dispatch loop), and synchronously
    // dispatching the next queued incoming Notify/CloseNotification re-enters the
    // manager mid-mutation — invalidating live references/indices and crashing
    // under a notification burst. The poll loop flushes the queued signal.
    m_object->emitSignal("NotificationClosed").onInterface(kInterface).withArguments(id, static_cast<uint32_t>(reason));
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
    // See emitClose(): never re-enter the bus dispatch from inside a manager
    // callback. Queue the signal; the poll loop flushes it.
    m_object->emitSignal("ActionInvoked").onInterface(kInterface).withArguments(id, actionKey);
  } catch (const sdbus::Error& e) {
    kLog.debug("notification #{}: ActionInvoked emit failed key='{}': {}", id, actionKey, e.what());
  }
}

std::tuple<std::string, std::string, std::string, std::string> NotificationService::onGetServerInformation() {
  return {"noctalia", "noctalia-dev", NOCTALIA_VERSION, "1.2"};
}
