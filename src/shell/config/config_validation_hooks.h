#pragma once

#include "config/config_service.h"

namespace nythera::config::shell {
  [[nodiscard]] ConfigService::MergedConfigValidator validator();
  [[nodiscard]] ConfigService::OverrideEffectivenessChecker overrideEffectivenessChecker();
}
