#pragma once

#include <functional>
#include <memory>

class SystemBus;

namespace sdbus {
  class IProxy;
}

class LogindService {
public:
  using PrepareForSleepCallback = std::function<void(bool sleeping)>;
  using SessionLockCallback = std::function<void()>;

  explicit LogindService(SystemBus& bus);
  ~LogindService();

  void setPrepareForSleepCallback(PrepareForSleepCallback callback);
  void setLockCallback(SessionLockCallback callback);
  void setUnlockCallback(SessionLockCallback callback);

  void setSessionLockIntegrationEnabled(bool enabled);
  // Sleep-delay inhibit for lock-before-suspend. Released when session lock integration is off.
  void setLockBeforeSuspendEnabled(bool enabled);
  void setSessionLockedHint(bool locked);

  bool supportsIdleInhibit() const noexcept;
  bool hasIdleInhibit() const noexcept;
  bool acquireIdleInhibit();
  void releaseIdleInhibit();

  bool acquireSleepDelayInhibit();
  void releaseSleepDelayInhibit();

private:
  void ensureSessionLockMonitor();

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_managerProxy;
  std::unique_ptr<sdbus::IProxy> m_sessionProxy;
  bool m_sessionLockIntegrationEnabled = false;
  PrepareForSleepCallback m_prepareForSleepCallback;
  SessionLockCallback m_lockCallback;
  SessionLockCallback m_unlockCallback;
  int m_idleInhibitFd = -1;
  int m_sleepDelayInhibitFd = -1;
};
