#pragma once

#include "cli/schema.h"

#include <array>

namespace noctalia::cli {

  inline constexpr std::array kDmenuFlags{
      Flag{"--prompt", "-p", "<text>", "Set the launcher prompt", {}, {}, false, false},
  };
  inline constexpr Command kDmenuCmd{
      "dmenu",
      "Read launcher choices from stdin",
      "Reads newline-separated items from stdin, presents them in the launcher,\n"
      "and prints the selection to stdout.",
      {},
      kDmenuFlags,
      {},
      {},
      false,
  };

} // namespace noctalia::cli
