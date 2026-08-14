#pragma once

#include "cli/schema.h"
#include "theme/scheme.h"

#include <array>
#include <string_view>

namespace noctalia::cli {

  inline constexpr std::array<std::string_view, 2> kThemeModeChoices{"dark", "light"};
  inline constexpr std::array kThemeFlags{
      Flag{"--scheme", {}, "<name>", "Color generation scheme", theme::kSchemeNames, "m3-tonal-spot", false, false},
      Flag{"--dark", {}, {}, "Emit only the dark variant", {}, {}, false, false},
      Flag{"--light", {}, {}, "Emit only the light variant", {}, {}, false, false},
      Flag{"--both", {}, {}, "Emit both variants under dark/light keys", {}, {}, false, false},
      Flag{"--pure-black", {}, {}, "Re-anchor the dark surface ramp to true black (OLED)", {}, {}, false, false},
      Flag{"--theme-json", {}, "<file>", "Load precomputed dark/light token maps from JSON", {}, {}, false, false},
      Flag{{}, "-o", "<file>", "Write JSON to file instead of stdout", {}, {}, false, false},
      Flag{"--render", "-r", "<in:out>", "Render a template file to an output path", {}, {}, true, false},
      Flag{"--config", "-c", "<file>", "Process a TOML template config file", {}, {}, false, false},
      Flag{"--builtin-config", {}, {}, "Process the shipped built-in template catalog", {}, {}, false, false},
      Flag{
          "--list-templates",
          {},
          {},
          "List built-in, cached community, and configured user templates",
          {},
          {},
          false,
          false
      },
      Flag{"--default-mode", {}, "<mode>", "Template default mode", kThemeModeChoices, "dark", false, false},
  };
  inline constexpr std::array kThemePositionals{
      Positional{"image", "Image used to generate the palette", {}, false, false, false},
  };
  inline constexpr Command kThemeCmd{
      "theme",
      "Generate a color palette from an image",
      "Generate a color palette from an image. Material You and custom\nschemes produce very different results.",
      {},
      kThemeFlags,
      kThemePositionals,
      {},
      false,
  };

} // namespace noctalia::cli
