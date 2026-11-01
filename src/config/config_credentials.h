#pragma once

#include "core/toml.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace noctalia::config {

  // How `noctalia config export` writes values that are credentials.
  enum class CredentialExportMode : std::uint8_t {
    Keep,  // verbatim
    Omit,  // key removed
    Blank, // key kept with an empty value of the same TOML type
  };

  [[nodiscard]] std::optional<CredentialExportMode> credentialExportModeFromString(std::string_view name);

  // Applies `mode` to every credential in a merged or effective config root: calendar account
  // `server_url`, git plugin source `location`, and plugin settings an enabled plugin's manifest
  // declares with `secret = true` (plugin_settings, bar widgets, desktop and lockscreen widgets).
  void applyCredentialExportMode(toml::table& root, CredentialExportMode mode);

} // namespace noctalia::config
