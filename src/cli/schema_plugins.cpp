#include "cli/schema.h"

namespace noctalia::plugins {

  constexpr cli_schema::CliPositional kLintPositionals[] = {
      {.name = "path", .description = "plugin directory or directory of plugins", .required = false}
  };

  constexpr cli_schema::CliFlag kLintFlags[] = {
      {.longName = "--help", .shortName = "-h", .description = "Print help for lint"},
  };

  extern constexpr cli_schema::CliCommand kLintCmd = {
      .name = "lint",
      .summary = "Cross-check plugin settings against getConfig() calls",
      .flags = kLintFlags,
      .positionals = kLintPositionals,
  };

  constexpr std::string_view kPluginsSubcommands[] = {"lint"};

  constexpr cli_schema::CliPositional kPluginsPositionals[] = {
      {.name = "command", .description = "subcommand to run (lint)", .choices = kPluginsSubcommands, .required = true}
  };

  constexpr cli_schema::CliFlag kPluginsFlags[] = {
      {.longName = "--help", .shortName = "-h", .description = "Print help for plugins"},
  };

  constexpr cli_schema::CliCommand kPluginsCmd = {
      .name = "plugins",
      .summary = "Offline tools for plugin authors",
      .flags = kPluginsFlags,
      .positionals = kPluginsPositionals,
      .extraArgError = "error: usage: noctalia plugins <command> [paths]"
  };

} // namespace noctalia::plugins
