#pragma once

#include "cli/schema.h"
#include <span>

namespace noctalia::ipc {

  std::span<const cli_schema::CliCommand> getIpcSubcommands();

  extern const cli_schema::CliCommand kNotificationCmd;
  extern const cli_schema::CliCommand kMsgCmd;

} // namespace noctalia::ipc
