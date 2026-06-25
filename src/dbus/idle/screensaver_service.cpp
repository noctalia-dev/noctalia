#include "dbus/idle/screensaver_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("screensaver");

  const sdbus::ServiceName kBusName{"org.freedesktop.ScreenSaver"};
  const sdbus::ServiceName kLogindBusName{"org.freedesktop.login1"};
  const sdbus::ObjectPath kLogindObjectPath{"/org/freedesktop/login1"};
  constexpr auto kInterface = "org.freedesktop.ScreenSaver";
  constexpr auto kLogindManagerInterface = "org.freedesktop.login1.Manager";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
  constexpr auto kFileExistsError = "org.freedesktop.DBus.Error.FileExists";

  constexpr int kMaxEventsPerDispatch = 16;
  constexpr auto kMaxDispatchBudget = std::chrono::milliseconds(4);

  const sdbus::ObjectPath kObjectPaths[] = {
      sdbus::ObjectPath{"/org/freedesktop/ScreenSaver"},
      sdbus::ObjectPath{"/ScreenSaver"},
  };

} // namespace

ScreenSaverService::ScreenSaverService(SystemBus* systemBus) {
  try {
    m_connection = sdbus::createSessionBusConnection(kBusName);
    registerScreenSaver();
  } catch (const sdbus::Error& e) {
    if (e.getName() != kFileExistsError) {
      kLog.warn("screensaver D-Bus registration failed: {}", e.what());
    }
    m_connection.reset();
    m_objects.clear();
    m_dbusProxy.reset();
  } catch (const std::exception& e) {
    kLog.warn("screensaver D-Bus registration failed: {}", e.what());
    m_connection.reset();
    m_objects.clear();
    m_dbusProxy.reset();
  }

  registerLogindIdleMonitor(systemBus);
  if (!m_active && m_logindProxy == nullptr) {
    kLog.warn("no idle inhibit sources available");
  }
}

ScreenSaverService::~ScreenSaverService() {
  m_logindProxy.reset();
  m_dbusProxy.reset();
  m_objects.clear();
  m_connection.reset();
}

void ScreenSaverService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

sdbus::IConnection::PollData ScreenSaverService::getPollData() const {
  if (m_connection == nullptr) {
    return {};
  }
  return m_connection->getEventLoopPollData();
}

void ScreenSaverService::processPendingEvents() {
  if (m_connection == nullptr) {
    m_hasPendingEvents = false;
    return;
  }

  m_hasPendingEvents = false;
  const auto batchStart = std::chrono::steady_clock::now();
  for (int processed = 0; processed < kMaxEventsPerDispatch; ++processed) {
    if (!m_connection->processPendingEvent()) {
      return;
    }
    if (processed + 1 >= kMaxEventsPerDispatch || std::chrono::steady_clock::now() - batchStart >= kMaxDispatchBudget) {
      m_hasPendingEvents = true;
      return;
    }
  }
}

void ScreenSaverService::registerScreenSaver() {
  if (m_connection == nullptr) {
    return;
  }

  m_dbusProxy = sdbus::createProxy(
      *m_connection, sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"}
  );
  m_dbusProxy->uponSignal("NameOwnerChanged")
      .onInterface("org.freedesktop.DBus")
      .call([this](const std::string& /*name*/, const std::string& oldOwner, const std::string& newOwner) {
        if (!newOwner.empty()) {
          return;
        }
        const std::size_t removed = unregisterOwnerCookies(oldOwner);
        if (removed > 0) {
          kLog.info("screensaver: cleared {} inhibit(s) after client disconnect", removed);
        }
      });

  for (const auto& path : kObjectPaths) {
    try {
      auto object = sdbus::createObject(*m_connection, path);
      object
          ->addVTable(
              sdbus::registerMethod("Inhibit")
                  .withInputParamNames("application_name", "reason_for_inhibit")
                  .withOutputParamNames("cookie")
                  .implementedAs([this, objectPtr = object.get()](std::string app, std::string reason) {
                    return onInhibit(
                        std::move(app), std::move(reason), objectPtr->getCurrentlyProcessedMessage().getSender()
                    );
                  }),
              sdbus::registerMethod("UnInhibit")
                  .withInputParamNames("cookie")
                  .implementedAs([this, objectPtr = object.get()](std::uint32_t cookie) {
                    onUninhibit(cookie, objectPtr->getCurrentlyProcessedMessage().getSender());
                  })
          )
          .forInterface(kInterface);
      m_objects.push_back(std::move(object));
    } catch (const std::exception& e) {
      kLog.warn("failed to register ScreenSaver at {}: {}", std::string{path}, e.what());
    }
  }

  if (m_objects.empty()) {
    throw sdbus::Error(sdbus::Error::Name{kFileExistsError}, "no ScreenSaver object paths registered");
  }

  m_active = true;
  kLog.info("listening on org.freedesktop.ScreenSaver ({} object path(s))", m_objects.size());
}

void ScreenSaverService::registerLogindIdleMonitor(SystemBus* systemBus) {
  if (systemBus == nullptr) {
    return;
  }
  if (!systemBus->nameHasOwner("org.freedesktop.login1")) {
    kLog.debug("logind unavailable; systemd idle inhibit monitor disabled");
    return;
  }

  try {
    m_logindProxy = sdbus::createProxy(systemBus->connection(), kLogindBusName, kLogindObjectPath);
    m_logindProxy->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this](
                  const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changedProperties,
                  const std::vector<std::string>& /*invalidatedProperties*/
              ) {
          if (interfaceName != kLogindManagerInterface) {
            return;
          }
          const auto it = changedProperties.find("BlockInhibited");
          if (it == changedProperties.end()) {
            return;
          }
          applyLogindBlockInhibited(it->second.get<std::string>());
        });

    const auto blockInhibited =
        m_logindProxy->getProperty("BlockInhibited").onInterface(kLogindManagerInterface).get<std::string>();
    applyLogindBlockInhibited(blockInhibited);
    kLog.info("logind idle inhibit monitor active");
  } catch (const std::exception& e) {
    kLog.warn("logind idle inhibit monitor disabled: {}", e.what());
    m_logindProxy.reset();
  }
}

void ScreenSaverService::applyLogindBlockInhibited(const std::string& inhibits) {
  const std::string tagged = ":" + inhibits + ":";
  const bool idleBlocked = tagged.contains(":idle:");
  if (idleBlocked && !m_logindIdleInhibited) {
    m_logindIdleInhibited = true;
    onInhibitDelta(1);
    kLog.info("systemd idle inhibit active");
  } else if (!idleBlocked && m_logindIdleInhibited) {
    m_logindIdleInhibited = false;
    onInhibitDelta(-1);
    kLog.info("systemd idle inhibit released");
  }
}

std::uint32_t ScreenSaverService::onInhibit(std::string app, std::string reason, const char* sender) {
  kLog.debug("screensaver inhibit from {} ({}): {}", app, sender != nullptr ? sender : "?", reason);
  onInhibitDelta(1);

  const auto cookie = m_nextCookieId++;
  m_cookies.push_back(
      InhibitCookie{
          .cookie = cookie,
          .app = std::move(app),
          .reason = std::move(reason),
          .ownerId = sender != nullptr ? std::string(sender) : std::string{},
      }
  );
  return cookie;
}

void ScreenSaverService::onUninhibit(std::uint32_t cookie, const char* sender) {
  const auto it = std::ranges::find(m_cookies, cookie, &InhibitCookie::cookie);
  if (it == m_cookies.end()) {
    kLog.warn("screensaver uninhibit: unknown cookie {} from {}", cookie, sender != nullptr ? sender : "?");
    return;
  }

  kLog.debug("screensaver uninhibit from {} ({}): {}", it->app, sender != nullptr ? sender : "?", it->reason);
  m_cookies.erase(it);
  onInhibitDelta(-1);
}

void ScreenSaverService::onInhibitDelta(std::int64_t delta) {
  m_inhibitLocks = std::max<std::int64_t>(0, m_inhibitLocks + delta);
  if (m_changeCallback) {
    m_changeCallback(m_inhibitLocks);
  }
}

std::size_t ScreenSaverService::unregisterOwnerCookies(const std::string& ownerId) {
  if (ownerId.empty()) {
    return 0;
  }

  std::size_t removed = 0;
  for (auto it = m_cookies.begin(); it != m_cookies.end();) {
    if (it->ownerId != ownerId) {
      ++it;
      continue;
    }
    ++removed;
    it = m_cookies.erase(it);
    onInhibitDelta(-1);
  }
  return removed;
}
