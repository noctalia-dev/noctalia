#pragma once

#include "config/config_types.h"

#include <functional>
#include <mutex>
#include <string_view>

class CompositorPlatform;
class LockScreen;

struct SessionActionHooks {
  std::function<bool()> onLogout;
  std::function<bool()> onReboot;
  std::function<bool()> onShutdown;
  // Armed before Noctalia-initiated suspend so PrepareForSleep can skip lock-before-sleep.
  std::function<void()> onBeforePlainSuspend;
  std::function<void()> onPlainSuspendAborted;
};

class SessionActionRunner {
public:
  explicit SessionActionRunner(CompositorPlatform& platform, LockScreen& lockScreen, SessionActionHooks hooks = {});

  void setHooks(SessionActionHooks hooks);
  void setPowerConfig(const ShellSessionConfig::ShellSessionPowerConfig& power);
  void invoke(const SessionPanelActionConfig& cfg) const;
  bool lock() const;
  bool requestSuspendDetached() const;
  bool requestRebootDetached() const;
  bool requestRebootUefiDetached() const;
  bool requestShutdownDetached() const;
  bool lockThenSuspendDetached() const;

private:
  std::function<bool()> hookFor(std::string_view action) const;
  bool suspendBlocking() const;
  bool rebootBlocking() const;
  bool shutdownBlocking() const;

  CompositorPlatform& m_platform;
  LockScreen& m_lockScreen;
  SessionActionHooks m_hooks;

  // Session power command resolution is used from UI threads and worker threads
  // (panel/IPC/lock-and-suspend), so keep it internally synchronized.
  mutable std::mutex m_powerMutex;
  mutable std::optional<std::string> m_suspendCommandOverride;
  mutable std::optional<std::string> m_rebootCommandOverride;
  mutable std::optional<std::string> m_shutdownCommandOverride;

  // Auto-detection cache: where to start scanning fallback variants next time.
  mutable std::optional<std::size_t> m_cachedSuspendAutoStartIdx;
  mutable std::optional<std::size_t> m_cachedRebootAutoStartIdx;
  mutable std::optional<std::size_t> m_cachedRebootUefiAutoStartIdx;
  mutable std::optional<std::size_t> m_cachedShutdownAutoStartIdx;
};
