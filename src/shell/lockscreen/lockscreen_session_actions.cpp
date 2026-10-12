#include "shell/lockscreen/lockscreen_session_actions.h"

#include "shell/session/session_action_meta.h"
#include "util/string_utils.h"

#include <algorithm>

namespace {

  [[nodiscard]] bool hasEnabledSuspend(const std::vector<SessionPanelActionConfig>& actions) {
    return std::ranges::any_of(actions, [](const SessionPanelActionConfig& row) {
      return row.enabled && row.action == "suspend";
    });
  }

  [[nodiscard]] std::vector<SessionPanelActionConfig>
  filterActions(const std::vector<SessionPanelActionConfig>& actions) {
    const bool explicitSuspend = hasEnabledSuspend(actions);
    std::vector<SessionPanelActionConfig> out;
    out.reserve(actions.size());

    for (const auto& row : actions) {
      if (!row.enabled || !session_action::isKnown(row.action) || row.action == "lock") {
        continue;
      }
      if (row.action == "command" && (!row.command.has_value() || StringUtils::trim(*row.command).empty())) {
        continue;
      }
      if (row.action == "lock_and_suspend"
          && (explicitSuspend || (row.command.has_value() && !StringUtils::trim(*row.command).empty()))) {
        continue;
      }
      out.push_back(row);
    }
    return out;
  }

} // namespace

std::vector<SessionPanelActionConfig> lockscreen::resolveSessionActions(
    const std::vector<SessionPanelActionConfig>& configured, const std::vector<SessionPanelActionConfig>& defaults
) {
  auto resolved = filterActions(configured);
  if (resolved.empty()) {
    resolved = filterActions(defaults);
  }
  return resolved;
}
