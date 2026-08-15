#include "shell/session/session_action_meta.h"

#include <string_view>

namespace session_action {

  namespace {

    [[nodiscard]] bool isBuiltinAction(std::string_view action) {
      return action == "lock"
          || action == "logout"
          || action == "suspend"
          || action == "lock_and_suspend"
          || action == "reboot"
          || action == "shutdown";
    }

  } // namespace

  bool isKnown(std::string_view action) {
    return action == "lock"
        || action == "logout"
        || action == "suspend"
        || action == "lock_and_suspend"
        || action == "reboot"
        || action == "shutdown"
        || action == "command";
  }

  const char* labelKey(std::string_view action) {
    if (action == "lock") {
      return "session.actions.lock";
    }
    if (action == "logout") {
      return "session.actions.logout";
    }
    if (action == "suspend") {
      return "session.actions.suspend";
    }
    if (action == "lock_and_suspend") {
      return "session.actions.lock-and-suspend";
    }
    if (action == "reboot") {
      return "session.actions.reboot";
    }
    if (action == "shutdown") {
      return "session.actions.shutdown";
    }
    return "session.actions.custom";
  }

  const char* defaultGlyph(std::string_view action) {
    if (action == "lock") {
      return "lock";
    }
    if (action == "logout") {
      return "logout";
    }
    if (action == "suspend") {
      return "suspend";
    }
    if (action == "lock_and_suspend") {
      return "suspend";
    }
    if (action == "reboot") {
      return "reboot";
    }
    if (action == "shutdown") {
      return "shutdown";
    }
    return "terminal";
  }

  std::optional<std::string_view> canonicalActionName(std::string_view ipcOrConfigAction) {
    if (ipcOrConfigAction == "lock-and-suspend") {
      return std::string_view{"lock_and_suspend"};
    }
    if (isBuiltinAction(ipcOrConfigAction)) {
      return ipcOrConfigAction;
    }
    return std::nullopt;
  }

} // namespace session_action
