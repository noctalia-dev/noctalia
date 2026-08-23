#pragma once

#include "cli/schema.h"

#include <array>

namespace noctalia::cli {

  inline constexpr std::array kPluginsLintPositionals{
      Positional{
          "path",
          "plugin directory, or a directory of plugins; defaults to .",
          {},
          false,
          true,
          false,
      },
  };
  inline constexpr Command kPluginsLintCmd{
      "lint",
      "Cross-check declared settings against plugin code",
      "Cross-check each plugin's declared settings against getConfig() calls in entries and static modules.\n"
      "Reports settings read but not declared in plugin.toml (a runtime loud miss),\n"
      "obsolete entry-specific getConfig aliases, settings declared but never read,\n"
      "and entries pointing at a missing file.\n"
      "A path may be a plugin directory or a directory of plugins. Defaults to '.'.\n"
      "Exits 1 if any error-level problem is found.",
      {},
      {},
      kPluginsLintPositionals,
      {},
      false,
  };
  inline constexpr std::array kPluginsSubcommands{kPluginsLintCmd};
  inline constexpr Command kPluginsCmd{
      "plugins",
      "Offline plugin author tools",
      "Offline tools for plugin authors (no running shell required).\n"
      "To manage installed plugins on the running instance, use 'noctalia msg plugins'.",
      {},
      {},
      {},
      kPluginsSubcommands,
      false,
  };

} // namespace noctalia::cli
