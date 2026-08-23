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

  [[nodiscard]] SessionPanelActionConfig builtinAction(std::string_view action) {
    return SessionPanelActionConfig{.action = std::string(action)};
  }

  [[nodiscard]] std::string unknownSessionActionError(std::string_view action) {
    return "error: unknown session action \""
        + std::string(action)
        + "\" (try: lock, suspend, lock-and-suspend, logout, reboot, shutdown)\n";
  }

} // namespace

void registerSessionIpc(IpcService& ipc, SessionActionRunner& runner, LockScreen& lockScreen, ConfigService& config) {
  const auto dispatch = [&runner, &lockScreen, &config](const std::string& args) -> std::string {
    const auto parts = noctalia::ipc::splitWords(args);
    if (parts.empty()) {
      return "error: session requires <lock|suspend|lock-and-suspend|logout|reboot|shutdown>\n";
    }

    const std::string& ipcAction = parts[0];
    const auto action = session_action::canonicalActionName(ipcAction);
    if (!action.has_value()) {
      return unknownSessionActionError(ipcAction);
    }

    if (*action == "lock") {
      if (!config.isLockScreenEnabled()) {
        return "error: lock screen disabled\n";
      }
      if (lockScreen.lock()) {
        return "ok\n";
      }
      return "error: lock screen unavailable\n";
    }
    if (*action == "lock_and_suspend") {
      if (!config.isLockScreenEnabled()) {
        runner.invoke(builtinAction("suspend"));
        return "ok\n";
      }
      runner.invoke(builtinAction(*action));
      return "ok\n";
    }

    runner.invoke(builtinAction(*action));
    return "ok\n";
  };

  ipc.bind(noctalia::cli::msg::session, dispatch);
}
