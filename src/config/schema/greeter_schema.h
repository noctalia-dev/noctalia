#pragma once

#include "config/schema/diagnostics.h"
#include "config/schema/widget_schema.h"
#include "config/toml.h"
#include "shell/greeter/greeter_profile.h"

#include <optional>
#include <string_view>

namespace noctalia::config::schema {
  // Invalid profiles are diagnosed as a unit, never partially accepted with dropped instances.
  std::optional<GreeterProfile>
  readGreeterConfig(const toml::table& table, std::string_view path, Diagnostics& diagnostics);
  WidgetSettingSchema greeterWidgetSettingSchema(std::string_view type);
  toml::table writeGreeterConfig(const GreeterProfile& config);
} // namespace noctalia::config::schema
