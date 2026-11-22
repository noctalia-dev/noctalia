#pragma once

#include "config/greeter_config.h"
#include "config/toml.h"

#include <expected>
#include <filesystem>
#include <string>

namespace greeter::detail {
  // Produces the helper's visual tables and PNG manifest without changing the saved profile.
  std::expected<toml::table, std::string>
  stageCustomization(const std::filesystem::path& staging, ShellGreeterConfig profile);
} // namespace greeter::detail
