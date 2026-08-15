#pragma once

#include "cli/schema.h"

#include <array>
#include <string_view>

namespace noctalia::cli {

  inline constexpr std::array<std::string_view, 9> kFirefoxThemeActionChoices{
      "install", "uninstall", "update", "dark", "light", "auto", "host", "start", "help",
  };
  inline constexpr std::array kFirefoxThemePositionals{
      Positional{"action", "Action to perform", kFirefoxThemeActionChoices, true, false, false},
  };
  inline constexpr Command kFirefoxThemeCmd{
      "firefox-theme",
      "Manage Firefox theme integration",
      "Firefox theme host helpers (Pywalfox-compatible).\n\n"
      "Templates use post_action = \"firefox-theme\" after writing colors.json.\n"
      "Firefox still requires the Pywalfox browser extension.\n"
      "Multiple Firefox profiles are supported: each profile runs a host process,\n"
      "and theme pushes fan out to all of them.",
      {},
      {},
      kFirefoxThemePositionals,
      {},
      false,
  };

} // namespace noctalia::cli
