#pragma once

#include "config/greeter_config.h"
#include "config/schema/diagnostics.h"
#include "config/toml.h"

#include <optional>
#include <string_view>

namespace noctalia::config::schema {
  // Invalid profiles are diagnosed as a unit, never partially accepted with dropped instances.
  std::optional<ShellGreeterConfig>
  readGreeterConfig(const toml::table& table, std::string_view path, Diagnostics& diagnostics);
  toml::table writeGreeterConfig(const ShellGreeterConfig& config);
} // namespace noctalia::config::schema
