#pragma once

#include "config/config_types.h"
#include "core/toml.h"

#include <optional>
#include <string_view>

namespace noctalia::config {

  void seedBuiltinWidgets(Config& config);

  std::optional<WidgetSettingValue> readWidgetSettingValue(const toml::node& node);

  // Resolves one `[widget.<name>]` table against the config parsed so far. This
  // is the canonical bar-widget instance resolution used by runtime loading and
  // `config validate`.
  WidgetConfig
  readBarWidgetConfig(std::string_view widgetName, const toml::table& entryTable, const Config& baseConfig);

} // namespace noctalia::config
