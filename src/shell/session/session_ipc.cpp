#include "shell/session/session_ipc.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "shell/lockscreen/lock_screen.h"
#include "shell/session/session_action_meta.h"
#include "shell/session/session_action_runner.h"

#include <string>
#include <string_view>

namespace {

  [[nodiscard]] std::string unknownSessionActionError(std::string_view action) {
    return "error: unknown session action \""
        + std::string(action)
        + "\" (try: lock, suspend, lock-and-suspend, logout, reboot, shutdown)\n";
  }

  [[nodiscard]] std::string sessionActionUnavailableError(std::string_view action) {
    return "error: session action \"" + std::string(action) + "\" is disabled or not configured\n";
  }

  [[nodiscard]] std::optional<SessionPanelActionConfig>
  resolveIpcAction(const ConfigService& config, std::string_view ipcAction) {
    const auto canonical = session_action::canonicalActionName(ipcAction);
    if (!canonical.has_value()) {
      return std::nullopt;
    }
    return session_action::resolveConfiguredAction(config.config().shell.session, *canonical);
  }

} // namespace

void registerSessionIpc(IpcService& ipc, SessionActionRunner& runner, LockScreen& lockScreen, ConfigService& config) {
  const auto dispatch = [&runner, &lockScreen, &config](const std::string& args) -> std::string {
    const auto parts = noctalia::ipc::splitWords(args);
    if (parts.empty()) {
      return "error: session requires <lock|suspend|lock-and-suspend|logout|reboot|shutdown>\n";
    }

    const std::string& ipcAction = parts[0];
    const auto resolved = resolveIpcAction(config, ipcAction);
    if (!resolved.has_value()) {
      if (session_action::canonicalActionName(ipcAction).has_value()) {
        return sessionActionUnavailableError(ipcAction);
      }
      return unknownSessionActionError(ipcAction);
    }

    const std::string_view action = resolved->action;
    if (action == "lock") {
      if (!config.isLockScreenEnabled()) {
        return "error: lock screen disabled\n";
      }
      if (lockScreen.lock()) {
        return "ok\n";
      }
      return "error: lock screen unavailable\n";
    }
    if (action == "lock_and_suspend") {
      if (!config.isLockScreenEnabled()) {
        if (const auto suspend = session_action::resolveConfiguredAction(config.config().shell.session, "suspend")) {
          runner.invoke(*suspend);
          return "ok\n";
        }
        return sessionActionUnavailableError("suspend");
      }
      runner.invoke(*resolved);
      return "ok\n";
    }

    runner.invoke(*resolved);
    return "ok\n";
  };

  ipc.registerHandler(
      "session", dispatch, "session <lock|suspend|lock-and-suspend|logout|reboot|shutdown>",
      "Run a built-in session action"
  );
}
