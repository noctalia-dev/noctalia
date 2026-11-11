#pragma once

#include "config/config_types.h"

#include <vector>

namespace lockscreen {

  std::vector<SessionPanelActionConfig> resolveSessionActions(
      const std::vector<SessionPanelActionConfig>& configured, const std::vector<SessionPanelActionConfig>& defaults
  );

} // namespace lockscreen
