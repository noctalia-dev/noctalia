#pragma once

#include <functional>
#include <memory>

class SessionBus;

namespace sdbus {
  class IProxy;
}

namespace compositors::kde {

  // Follows KDE's screen locker (org.freedesktop.ScreenSaver.ActiveChanged, served by kwin_wayland).
  // KWin has no ext-session-lock, so Noctalia never owns the lock on KDE; this keeps the session
  // lock hooks, idle manager and screen-time service in step with the real lock state anyway.
  class KdeLockWatcher {
  public:
    using Callback = std::function<void(bool locked)>;

    KdeLockWatcher(SessionBus& bus, Callback callback);
    ~KdeLockWatcher();

    KdeLockWatcher(const KdeLockWatcher&) = delete;
    KdeLockWatcher& operator=(const KdeLockWatcher&) = delete;

  private:
    std::unique_ptr<sdbus::IProxy> m_proxy;
    Callback m_callback;
  };

} // namespace compositors::kde
