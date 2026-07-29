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
};

class SessionActionRunner {
public:
  explicit SessionActionRunner(CompositorPlatform& platform, LockScreen& lockScreen, SessionActionHooks hooks = {});

  void setHooks(SessionActionHooks hooks);
  void setPowerConfig(const ShellSessionConfig::ShellSessionPowerConfig& power);
  void invoke(const SessionPanelActionConfig& cfg) const;
  [[nodiscard]] bool lock() const;
  [[nodiscard]] bool requestSuspendDetached() const;
  [[nodiscard]] bool requestRebootDetached() const;
  [[nodiscard]] bool requestShutdownDetached() const;
  [[nodiscard]] bool lockThenSuspendDetached() const;

private:
  [[nodiscard]] std::function<bool()> hookFor(std::string_view action) const;
  [[nodiscard]] bool suspendBlocking() const;
  [[nodiscard]] bool rebootBlocking() const;
  [[nodiscard]] bool shutdownBlocking() const;

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
  mutable std::optional<std::size_t> m_cachedShutdownAutoStartIdx;
};
