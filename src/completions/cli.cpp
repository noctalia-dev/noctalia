#include "completions/cli.h"
#include "completions_generated.h"

#include "completions_generated.h"

#include <cstdio>
#include <cstring>
#include <print>

namespace noctalia::completions {
  namespace {

    constexpr const char* kHelpText = "Usage: noctalia completions <bash|fish|zsh>\n"
                                      "\n"
                                      "Print a shell completion script to stdout.\n"
                                      "\n"
                                      "Examples:\n"
                                      "  source <(noctalia completions bash)\n"
                                      "  noctalia completions fish > ~/.config/fish/completions/noctalia.fish\n";

  } // namespace

  int runCli(int argc, char* argv[]) {
    if (argc < 3) {
      std::print(stderr, "{}", kHelpText);
      return 1;
    }

    if (std::strcmp(argv[2], "--help") == 0) {
      std::print("{}", kHelpText);
      return 0;
    }

    if (argc > 3) {
      std::println(stderr, "error: unexpected argument: {}", argv[3]);
      std::println(stderr, "Run 'noctalia completions --help' for usage.");
      return 1;
    }

    const char* shell = argv[2];

    const char* script = nullptr;
    if (std::strcmp(shell, "bash") == 0) {
      script = NOCTALIA_BASH_SCRIPT;
    } else if (std::strcmp(shell, "zsh") == 0) {
      script = NOCTALIA_ZSH_SCRIPT;
    } else if (std::strcmp(shell, "fish") == 0) {
      script = NOCTALIA_FISH_SCRIPT;
    } else {
      std::println(stderr, "error: unknown shell: {}", shell);
      std::println(stderr, "Run 'noctalia completions --help' for usage.");
      return 1;
    }

    std::fwrite(script, 1, std::strlen(script), stdout);
    return 0;
  }

} // namespace noctalia::completions
