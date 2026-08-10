#pragma once

#include "cli/schema.h"
#include "cli/schema_config.h"
#include "cli/schema_dmenu.h"
#include "cli/schema_firefox_theme.h"
#include "cli/schema_msg.h"
#include "cli/schema_plugins.h"
#include "cli/schema_theme.h"

#include <array>

namespace noctalia::cli {

  inline constexpr std::array<std::string_view, 3> kCompletionShellChoices{"bash", "fish", "zsh"};
  inline constexpr std::array kCompletionsPositionals{
      Positional{"shell", "Shell completion format", kCompletionShellChoices, true, false, false},
  };
  inline constexpr Command kCompletionsCmd{
      "completions",
      "Generate shell completion scripts",
      "Generate a completion script from the live CLI schema.\n\n"
      "Examples:\n"
      "  source <(noctalia completions bash)\n"
      "  noctalia completions fish > ~/.config/fish/completions/noctalia.fish",
      {},
      {},
      kCompletionsPositionals,
      {},
      false,
  };

  inline constexpr std::array kRootFlags{
      Flag{"--version", "-v", {}, "Show version information", {}, {}, false, false},
      Flag{"--daemon", "-d", {}, "Run in background", {}, {}, false, false},
  };

  inline constexpr std::array kRootSubcommands{
      kCompletionsCmd, kConfigCmd, kDmenuCmd, kFirefoxThemeCmd, kMsgCmd, kPluginsCmd, kThemeCmd,
  };

  inline constexpr Command kRootCmd{
      "noctalia",
      "A sleek, customizable desktop shell crafted for Wayland",
      {},
      "For more information and documentation, visit:\n  https://noctalia.dev",
      kRootFlags,
      {},
      kRootSubcommands,
      false,
  };

  static_assert(validateCommand(kRootCmd));

} // namespace noctalia::cli
