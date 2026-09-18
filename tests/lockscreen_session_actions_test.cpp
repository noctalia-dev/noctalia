#include "config/config_types.h"
#include "shell/lockscreen/lockscreen_session_actions.h"
#include "tests/test_check.h"

#include <string>
#include <utility>
#include <vector>

namespace {

  [[nodiscard]] SessionPanelActionConfig action(std::string name) {
    return SessionPanelActionConfig{.action = std::move(name)};
  }

  [[nodiscard]] std::vector<std::string> names(const std::vector<SessionPanelActionConfig>& actions) {
    std::vector<std::string> out;
    out.reserve(actions.size());
    for (const auto& row : actions) {
      out.push_back(row.action);
    }
    return out;
  }

} // namespace

int main() {
  const std::vector defaults = {
      action("lock"), action("logout"), action("lock_and_suspend"), action("reboot"), action("shutdown")
  };

  TEST_CHECK(
      names(lockscreen::resolveSessionActions(defaults, defaults))
      == std::vector<std::string>({"logout", "lock_and_suspend", "reboot", "shutdown"})
  );

  auto oldExplicit = defaults;
  oldExplicit[2].label = "Sleep safely";
  oldExplicit[2].glyph = "bed";
  oldExplicit[2].variant = SessionActionButtonVariant::Primary;
  const auto resolvedExplicit = lockscreen::resolveSessionActions(oldExplicit, defaults);
  TEST_CHECK(resolvedExplicit.size() == 4);
  TEST_CHECK(resolvedExplicit[1] == oldExplicit[2]);

  const std::vector withSuspend = {action("logout"), action("lock_and_suspend"), action("suspend"), action("shutdown")};
  TEST_CHECK(
      names(lockscreen::resolveSessionActions(withSuspend, defaults))
      == std::vector<std::string>({"logout", "suspend", "shutdown"})
  );

  auto disabledSuspend = action("suspend");
  disabledSuspend.enabled = false;
  const std::vector withDisabledSuspend = {action("lock_and_suspend"), disabledSuspend, action("reboot")};
  TEST_CHECK(
      names(lockscreen::resolveSessionActions(withDisabledSuspend, defaults))
      == std::vector<std::string>({"lock_and_suspend", "reboot"})
  );

  auto customLockAndSuspend = action("lock_and_suspend");
  customLockAndSuspend.command = "custom-sleep";
  const std::vector withCustomCommand = {customLockAndSuspend, action("shutdown")};
  TEST_CHECK(
      names(lockscreen::resolveSessionActions(withCustomCommand, defaults)) == std::vector<std::string>({"shutdown"})
  );

  auto emptyCommand = action("command");
  emptyCommand.command = "  ";
  const std::vector invalidOnly = {action("lock"), emptyCommand, action("unknown")};
  TEST_CHECK(
      names(lockscreen::resolveSessionActions(invalidOnly, defaults))
      == std::vector<std::string>({"logout", "lock_and_suspend", "reboot", "shutdown"})
  );

  return 0;
}
