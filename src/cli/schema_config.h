#pragma once

#include "cli/schema.h"

#include <array>
#include <string_view>

namespace noctalia::cli {

  inline constexpr std::array kConfigValidatePositionals{
      Positional{"path", "Config file or directory; defaults to the active config", {}, false, false, false},
  };
  inline constexpr Command kConfigValidateCmd{
      "validate",
      "Check config validity",
      "With no path, validates the merged configuration the way the shell loads it:\n"
      "  - every *.toml in the active config dir, then\n"
      "  - the state-dir settings.toml overrides.\n\n"
      "With a directory path, validates only that directory's *.toml files.\n"
      "With a file path, validates only that file.\n\n"
      "Reports TOML syntax errors, unknown sections/settings, and bad values\n"
      "(wrong type, out-of-range, invalid enum/color). Exits 1 if any error is found.",
      {},
      {},
      kConfigValidatePositionals,
      {},
      false,
  };

  inline constexpr std::array<std::string_view, 2> kConfigExportModeChoices{"merged", "full"};
  inline constexpr std::array<std::string_view, 3> kConfigExportCredentialChoices{"keep", "omit", "blank"};
  inline constexpr std::array kConfigExportFlags{
      Flag{
          "--credentials",
          {},
          "<mode>",
          "keep, omit, or blank credential values",
          kConfigExportCredentialChoices,
          "keep",
          false,
          false
      },
  };
  inline constexpr std::array kConfigExportPositionals{
      Positional{"mode", "Export mode (default: merged)", kConfigExportModeChoices, false, false, false},
  };
  inline constexpr Command kConfigExportCmd{
      "export",
      "Print the active config as TOML",
      "Prints TOML to stdout from the same config stack used by the shell:\n"
      "  - every *.toml in the active config dir, then\n"
      "  - the state-dir settings.toml overrides.\n\n"
      "Modes:\n"
      "  merged  Export merged user config only (default)\n"
      "  full    Export full effective config, including built-in defaults\n\n"
      "--credentials controls values that can hold credentials: calendar account\n"
      "server_url, git plugin source location, and plugin settings marked secret.\n"
      "  keep   Print them as configured (default)\n"
      "  omit   Leave the keys out\n"
      "  blank  Keep the keys with empty values",
      {},
      kConfigExportFlags,
      kConfigExportPositionals,
      {},
      false,
  };

  inline constexpr Command kConfigSettingsCountCmd{
      "settings-count",
      "Count Settings UI controls",
      "Counts one Settings UI row/control per SettingEntry: toggles, sliders, lists,\n"
      "and pickers. Dropdown options and SettingsWindow-only action buttons are not\n"
      "counted separately.",
      {},
      {},
      {},
      {},
      false,
  };

  inline constexpr std::array kConfigReplayFlags{
      Flag{"--target", {}, "<dir>", "Directory where replay files are written", {}, {}, false, true},
      Flag{"--flattened", {}, {}, "Write only merged_config.content as config.toml", {}, {}, false, false},
      Flag{"--force", {}, {}, "Remove an existing target directory before writing", {}, {}, false, false},
  };
  inline constexpr std::array kConfigReplayPositionals{
      Positional{"report.toml", "Support report to replay", {}, true, false, false},
  };
  inline constexpr Command kConfigReplayReportCmd{
      "replay-report",
      "Reconstruct config and state from a support report",
      "Reconstruct config-home/noctalia and state-home/noctalia from a support report.\n\n"
      "With --flattened, reconstruct a single config-home/noctalia/config.toml from the report's merged config.",
      {},
      kConfigReplayFlags,
      kConfigReplayPositionals,
      {},
      false,
  };

  inline constexpr std::array kConfigSubcommands{
      kConfigExportCmd,
      kConfigReplayReportCmd,
      kConfigSettingsCountCmd,
      kConfigValidateCmd,
  };
  inline constexpr Command kConfigCmd{
      "config", "Validate config and support/replay helpers", {}, {}, {}, {}, kConfigSubcommands, false,
  };

} // namespace noctalia::cli
