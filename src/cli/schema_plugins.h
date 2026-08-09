#pragma once

#include "cli/schema.h"

#include <string_view>

namespace noctalia::plugins {

  inline constexpr cli_schema::CliPositional kLintPositionals[] = { //
      {.name = "path", .description = "plugin directory or directory of plugins", .required = false}
  };

  inline constexpr cli_schema::CliFlag kLintFlags[] = {
      {.longName = "--help", .description = "Print help for lint"},
  };

  inline constexpr cli_schema::CliCommand kLintCmd = {
      .name = "lint",
      .summary = "Cross-check plugin settings against getConfig() calls",
      .flags = kLintFlags,
      .positionals = kLintPositionals,
  };

  inline constexpr std::string_view kPluginsSubcommands[] = {"lint"};

  inline constexpr cli_schema::CliPositional kPluginsPositionals[] = {
      {.name = "command", .description = "subcommand to run (lint)", .choices = kPluginsSubcommands, .required = true}
  };

  inline constexpr cli_schema::CliFlag kPluginsFlags[] = {
      {.longName = "--help", .description = "Print help for plugins"},
  };

  inline constexpr cli_schema::CliCommand kPluginsCmd = {
      .name = "plugins",
      .summary = "Offline tools for plugin authors",
      .flags = kPluginsFlags,
      .positionals = kPluginsPositionals,
      .extraArgError = "error: usage: noctalia plugins <command> [paths]"
  };

} // namespace noctalia::plugins
