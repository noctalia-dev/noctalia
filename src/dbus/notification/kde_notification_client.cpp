#include "dbus/notification/kde_notification_client.h"

#include "compositors/compositor_detect.h"
#include "core/log.h"
#include "dbus/notification/notification_service.h"
#include "dbus/session_bus.h"
#include "notification/notification_manager.h"

#include <map>
#include <string>
#include <vector>

namespace {
  constexpr Logger kLog("notification");

  const sdbus::ServiceName kPlasmaNotifications{"org.freedesktop.Notifications"};
  const sdbus::ObjectPath kPlasmaNotificationsPath{"/org/freedesktop/Notifications"};
  const sdbus::ObjectPath kWatcherPath{"/NotificationWatcher"};
  constexpr auto kFdoNotificationsInterface = "org.freedesktop.Notifications";
  constexpr auto kKdeNotificationManagerInterface = "org.kde.NotificationManager";
  constexpr auto kKdeNotificationWatcherInterface = "org.kde.NotificationWatcher";
} // namespace

KdeNotificationClient::KdeNotificationClient(SessionBus& bus, NotificationManager& manager)
    : m_bus(bus), m_manager(manager) {
  if (!compositors::isKde()) {
    throw sdbus::Error(
        sdbus::Error::Name{"org.freedesktop.DBus.Error.Failed"}, "KDE notification client requires KDE Plasma"
    );
  }

  m_watcherObject = sdbus::createObject(m_bus.connection(), kWatcherPath);
  m_watcherObject
      ->addVTable(
          sdbus::registerMethod("Notify")
              .withInputParamNames(
                  "id", "app_name", "replaces_id", "app_icon", "summary", "body", "actions", "hints", "timeout"
              )
              .implementedAs(
                  [this](
                      uint32_t id, const std::string& appName, uint32_t replacesId, const std::string& appIcon,
                      const std::string& summary, const std::string& body, const std::vector<std::string>& actions,
                      const std::map<std::string, sdbus::Variant>& hints, int32_t timeout
                  ) { onWatcherNotify(id, appName, replacesId, appIcon, summary, body, actions, hints, timeout); }
              ),
          sdbus::registerMethod("CloseNotification").withInputParamNames("id").implementedAs([this](uint32_t id) {
            onWatcherCloseNotification(id);
          })
      )
      .forInterface(kKdeNotificationWatcherInterface);

  m_plasmaProxy = sdbus::createProxy(m_bus.connection(), kPlasmaNotifications, kPlasmaNotificationsPath);

  try {
    m_plasmaProxy->callMethod("Inhibit")
        .onInterface(kFdoNotificationsInterface)
        .withArguments(
            std::string{}, std::string{"Noctalia handles notifications"}, std::map<std::string, sdbus::Variant>{}
        )
        .storeResultsTo(m_inhibitCookie);
  } catch (const sdbus::Error& e) {
    kLog.warn("KDE notification inhibit failed: {}", e.what());
    throw;
  }

  try {
    m_plasmaProxy->callMethod("RegisterWatcher").onInterface(kKdeNotificationManagerInterface);
  } catch (const sdbus::Error& e) {
    kLog.warn("KDE notification RegisterWatcher failed: {}", e.what());
    if (m_inhibitCookie != 0) {
      try {
        m_plasmaProxy->callMethod("UnInhibit").onInterface(kFdoNotificationsInterface).withArguments(m_inhibitCookie);
      } catch (const sdbus::Error& unInhibitError) {
        kLog.debug("KDE notification UnInhibit after RegisterWatcher failure failed: {}", unInhibitError.what());
      }
      m_inhibitCookie = 0;
    }
    throw;
  }

  m_manager.setActionInvokeCallback([this](uint32_t id, const std::string& actionKey) {
    proxyInvokeAction(id, actionKey);
  });
  m_manager.setCloseCallback([this](uint32_t id, CloseReason /*reason*/) {
    if (m_suppressPlasmaClose) {
      return;
    }
    proxyCloseNotification(id);
  });

  m_active = true;
}

KdeNotificationClient::~KdeNotificationClient() {
  m_manager.setCloseCallback(nullptr);
  m_manager.setActionInvokeCallback(nullptr);
  unregisterWatcher();

  if (m_inhibitCookie != 0 && m_plasmaProxy != nullptr) {
    try {
      m_plasmaProxy->callMethod("UnInhibit").onInterface(kFdoNotificationsInterface).withArguments(m_inhibitCookie);
    } catch (const sdbus::Error& e) {
      kLog.debug("KDE notification UnInhibit failed: {}", e.what());
    }
    m_inhibitCookie = 0;
  }

  if (m_watcherObject != nullptr) {
    try {
      m_watcherObject->unregister();
    } catch (const sdbus::Error& e) {
      kLog.debug("KDE notification watcher unregister failed: {}", e.what());
    }
  }

  m_active = false;
}

bool KdeNotificationClient::isHealthy() const { return m_active; }

void KdeNotificationClient::processExpired() {
  const std::vector<uint32_t> ids = m_manager.expiredIds();
  for (const uint32_t id : ids) {
    (void)m_manager.close(id, CloseReason::Expired);
  }
}

void KdeNotificationClient::onWatcherNotify(
    uint32_t id, const std::string& appName, uint32_t replacesId, const std::string& appIcon,
    const std::string& summary, const std::string& body, const std::vector<std::string>& actions,
    const std::map<std::string, sdbus::Variant>& hints, int32_t timeout
) {
  if (replacesId != 0) {
    (void)notification_dbus::ingestNotify(
        m_manager, appName, replacesId, appIcon, summary, body, actions, hints, timeout
    );
    return;
  }

  (void)m_manager.adoptExternal(
      id,
      NotificationRequest{
          .appName = appName,
          .summary = summary,
          .body = body,
          .urgency = notification_dbus::notifyUrgencyFromHints(hints),
          .timeout = normalizeNotifyExpireTimeout(timeout),
          .transient = notification_dbus::notifyTransientFromHints(hints),
          .actions = notification_dbus::sanitizeNotifyActions(actions),
          .icon = notification_dbus::notifyIcon(appName, appIcon, hints),
          .imageData = notification_dbus::notifyImageDataFromHints(hints),
          .category = notification_dbus::notifyCategoryFromHints(hints),
          .desktopEntry = notification_dbus::notifyDesktopEntryFromHints(hints),
      }
  );
}

void KdeNotificationClient::onWatcherCloseNotification(uint32_t id) {
  m_suppressPlasmaClose = true;
  (void)m_manager.close(id, CloseReason::ClosedByCall);
  m_suppressPlasmaClose = false;
}

void KdeNotificationClient::proxyCloseNotification(uint32_t id) {
  if (m_plasmaProxy == nullptr) {
    return;
  }
  try {
    m_plasmaProxy->callMethod("CloseNotification").onInterface(kFdoNotificationsInterface).withArguments(id);
  } catch (const sdbus::Error& e) {
    kLog.debug("KDE notification #{}: plasma CloseNotification failed: {}", id, e.what());
  }
}

void KdeNotificationClient::proxyInvokeAction(uint32_t id, const std::string& actionKey) {
  if (m_plasmaProxy == nullptr) {
    return;
  }
  try {
    m_plasmaProxy->callMethod("InvokeAction").onInterface(kFdoNotificationsInterface).withArguments(id, actionKey);
  } catch (const sdbus::Error& e) {
    kLog.debug("KDE notification #{}: plasma InvokeAction failed key='{}': {}", id, actionKey, e.what());
  }
}

void KdeNotificationClient::unregisterWatcher() {
  if (!m_active || m_plasmaProxy == nullptr) {
    return;
  }
  try {
    m_plasmaProxy->callMethod("UnRegisterWatcher").onInterface(kKdeNotificationManagerInterface);
  } catch (const sdbus::Error& e) {
    kLog.debug("KDE notification UnRegisterWatcher failed: {}", e.what());
  }
}
