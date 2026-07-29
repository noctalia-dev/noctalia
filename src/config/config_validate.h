#pragma once

#include "config/schema/diagnostics.h"
#include "core/toml.h"

#include <string_view>

namespace noctalia::config {

  // Validates an already merged and normalized effective config table. This is the
  // shared, side-effect-free semantic validation pass used by live reload and the CLI.
  [[nodiscard]] schema::Diagnostics validateMergedConfig(const toml::table& merged);

  // Loads and merges the config sources the same way ConfigService::loadAll does
  // (sorted *.toml in configDir, then the optional settings.toml overrides) and
  // validates the result against the declarative schema + the widget setting
  // schema. Returns every issue found: syntax errors, unknown sections/keys, and
  // type/enum/range/color problems. Errors mean the config is invalid; warnings
  // are advisory (e.g. clamped ranges). Does not construct a ConfigService.
  [[nodiscard]] schema::Diagnostics
  validateConfigSources(std::string_view configDir, std::string_view settingsTomlPath);

  // Validates exactly one TOML file. No config directory is scanned and no
  // settings.toml override layer is read.
  [[nodiscard]] schema::Diagnostics validateConfigFile(std::string_view path);

} // namespace noctalia::config
