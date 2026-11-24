#include "shell/config/config_validation_hooks.h"

#include "config/config_validate.h"
#include "shell/settings/widget_settings_registry.h"

namespace nythera::config::shell {
  ConfigService::MergedConfigValidator validator() { return nythera::config::validateMergedConfig; }

  ConfigService::OverrideEffectivenessChecker overrideEffectivenessChecker() {
    return [](const std::vector<std::string>& path, const Config& withOverride, const Config& withoutOverride) {
      if (path.size() >= 3 && path[0] == "widget") {
        return settings::widgetSettingOverrideIsEffective(path[1], path[2], withOverride, withoutOverride);
      }
      if (path.size() >= 3 && path[0] == "plugin_settings") {
        return settings::pluginSettingOverrideIsEffective(path[1], path[2], withOverride, withoutOverride);
      }
      return false;
    };
  }
} // namespace nythera::config::shell
