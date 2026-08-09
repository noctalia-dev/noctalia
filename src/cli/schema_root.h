#pragma once

#include "cli/schema.h"
#include "cli/schema_config.h"
#include "cli/schema_ipc.h"
#include "cli/schema_plugins.h"
#include "cli/schema_theme.h"

namespace noctalia::cli {

  inline constexpr cli_schema::CliCommand kRootSubcommands[] = {
      config::kConfigCmd,
      ipc::kMsgCmd,
      plugins::kPluginsCmd,
      theme::kThemeCmd,
  };

  inline constexpr cli_schema::CliCommand kRootCmd = {
      .name = "noctalia",
      .summary = "A sleek, customizable desktop shell crafted for Wayland",
      .subcommands = kRootSubcommands,
  };

} // namespace noctalia::cli
