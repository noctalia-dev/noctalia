#pragma once

#include "cli/schema.h"

#include <string_view>

namespace noctalia::config {

  inline constexpr const char* kConfigHelpText =
      "Usage: noctalia config <command> [options]\n"
      "\n"
      "Commands:\n"
      "  validate [path]\n"
      "      Check config validity: TOML syntax, unknown/misspelled settings, and bad\n"
      "      values. Defaults to the active config dir + state settings.toml. A directory\n"
      "      validates its *.toml files; a file validates only that file. Exit 1 on error.\n"
      "\n"
      "  export [merged|full]\n"
      "      Print the active config as TOML. Defaults to merged user config.\n"
      "\n"
      "  settings-count\n"
      "      Count Settings UI controls by registry, visibility state, and section.\n"
      "\n"
      "  replay-report <report.toml> --target <dir> [--force]\n"
      "      Reconstruct config-home/noctalia and state-home/noctalia from a support report.\n"
      "\n"
      "  replay-report <report.toml> --target <dir> --flattened [--force]\n"
      "      Reconstruct a single config-home/noctalia/config.toml from the report's merged config.\n";

  inline constexpr const char* kValidateHelpText =
      "Usage: noctalia config validate [path]\n"
      "\n"
      "With no path, validates the merged configuration the way the shell loads it:\n"
      "  - every *.toml in the active config dir, then\n"
      "  - the state-dir settings.toml overrides.\n"
      "\n"
      "With a directory path, validates only that directory's *.toml files.\n"
      "With a file path, validates only that file.\n"
      "\n"
      "Reports TOML syntax errors, unknown sections/settings, and bad values\n"
      "(wrong type, out-of-range, invalid enum/color). Exits 1 if any error is found.\n";

  inline constexpr const char* kReplayHelpText =
      "Usage: noctalia config replay-report <report.toml> --target <dir> [--flattened] [--force]\n"
      "\n"
      "Options:\n"
      "  --target <dir>  Directory where replay files are written\n"
      "  --flattened     Write only merged_config.content as config.toml\n"
      "  --force         Remove an existing target directory before writing\n";

  inline constexpr const char* kExportHelpText =
      "Usage: noctalia config export [merged|full]\n"
      "\n"
      "Prints TOML to stdout from the same config stack used by the shell:\n"
      "  - every *.toml in the active config dir, then\n"
      "  - the state-dir settings.toml overrides.\n"
      "\n"
      "Modes:\n"
      "  merged  Export merged user config only (default)\n"
      "  full    Export full effective config, including built-in defaults\n";

  inline constexpr const char* kSettingsCountHelpText =
      "Usage: noctalia config settings-count\n"
      "\n"
      "Counts one Settings UI row/control per SettingEntry: toggles, sliders, lists,\n"
      "and pickers. Dropdown options and SettingsWindow-only action buttons are not\n"
      "counted separately.\n";

  inline constexpr std::string_view kExportChoices[] = {"merged", "full"};

  inline constexpr cli_schema::CliPositional kValidatePositionals[] = {
      {.name = "path", .description = "config file or directory", .required = false},
  };

  inline constexpr cli_schema::CliPositional kExportPositionals[] = {
      {.name = "mode",
       .description = "merged or full",
       .choices = kExportChoices,
       .required = false,
       .invalidChoiceError = "expected merged or full"},
  };

  inline constexpr cli_schema::CliPositional kReplayPositionals[] = {
      {.name = "report.toml",
       .description = "support report to replay",
       .required = true,
       .missingError = "missing report path"},
  };

  inline constexpr cli_schema::CliFlag kReplayFlags[] = {
      {.longName = "--target",
       .description = "target directory",
       .takesValue = true,
       .required = true,
       .missingValueError = "--target requires a directory",
       .missingFlagError = "missing --target <dir>"},
      {.longName = "--flattened", .description = "write flattened config.toml"},
      {.longName = "--force", .description = "overwrite existing target"},
      {.longName = "--help", .description = "show help message"},
  };

  inline constexpr cli_schema::CliCommand kValidateCmd = {
      .name = "validate",
      .summary = "Check config validity",
      .positionals = kValidatePositionals,
  };

  inline constexpr cli_schema::CliCommand kExportCmd = {
      .name = "export",
      .summary = "Print the active config as TOML",
      .positionals = kExportPositionals,
  };

  inline constexpr cli_schema::CliCommand kSettingsCountCmd = {
      .name = "settings-count",
      .summary = "Count Settings UI controls",
  };

  inline constexpr cli_schema::CliCommand kReplayReportCmd = {
      .name = "replay-report",
      .summary = "Reconstruct config from a support report",
      .flags = kReplayFlags,
      .positionals = kReplayPositionals,
  };

  inline constexpr cli_schema::CliCommand kConfigSubcommands[] = {
      kValidateCmd, kExportCmd, kSettingsCountCmd, kReplayReportCmd
  };

  inline constexpr cli_schema::CliCommand kConfigCmd = {
      .name = "config",
      .summary = "Manage shell configuration",
      .helpText = kConfigHelpText,
      .subcommands = kConfigSubcommands
  };

} // namespace noctalia::config
