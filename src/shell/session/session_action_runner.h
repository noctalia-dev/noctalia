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
  [[nodiscard]] bool lock() const;
  /// `behavior`: `suspend` | `hibernate` | `suspend-then-hibernate`.
  [[nodiscard]] bool requestSuspendDetached(std::string_view behavior = "suspend") const;
  [[nodiscard]] bool requestRebootDetached() const;
  [[nodiscard]] bool requestShutdownDetached() const;
  /// `behavior`: `suspend` | `hibernate` | `suspend-then-hibernate`.
  [[nodiscard]] bool lockThenSuspendDetached(std::string_view behavior = "suspend") const;

private:
  [[nodiscard]] std::function<bool()> hookFor(std::string_view action) const;
  // Per-behavior auto-detection cache for the suspend command variant scan.
  [[nodiscard]] std::optional<std::size_t>& suspendCacheFor(std::string_view behavior) const;
  [[nodiscard]] bool suspendBlocking(std::string_view behavior = "suspend") const;
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
  mutable std::optional<std::size_t> m_cachedHibernateAutoStartIdx;
  mutable std::optional<std::size_t> m_cachedSuspendThenHibernateAutoStartIdx;
  mutable std::optional<std::size_t> m_cachedRebootAutoStartIdx;
  mutable std::optional<std::size_t> m_cachedShutdownAutoStartIdx;
};
