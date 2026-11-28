#include "compositors/kde/kde_lock_watcher.h"

#include "core/log.h"
#include "dbus/session_bus.h"

#include <sdbus-c++/sdbus-c++.h>

namespace compositors::kde {

  namespace {
    constexpr Logger kLog("kde-lock");
    const sdbus::ServiceName kScreenSaverService{"org.freedesktop.ScreenSaver"};
    const sdbus::ObjectPath kScreenSaverPath{"/ScreenSaver"};
    constexpr const char* kScreenSaverInterface = "org.freedesktop.ScreenSaver";
  } // namespace

  KdeLockWatcher::KdeLockWatcher(SessionBus& bus, Callback callback) : m_callback(std::move(callback)) {
    try {
      m_proxy = sdbus::createProxy(bus.connection(), kScreenSaverService, kScreenSaverPath);
      m_proxy->uponSignal("ActiveChanged").onInterface(kScreenSaverInterface).call([this](bool active) {
        kLog.info("KDE screen locker {}", active ? "locked" : "unlocked");
        if (m_callback) {
          m_callback(active);
        }
      });
    } catch (const sdbus::Error& e) {
      kLog.warn("cannot watch the KDE screen locker: {}", e.what());
      m_proxy.reset();
    }
  }

  KdeLockWatcher::~KdeLockWatcher() = default;

} // namespace compositors::kde
