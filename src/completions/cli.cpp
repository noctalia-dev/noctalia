
#include "cli/parse.h"

#include <cstdio>
#include <cstring>
#include <print>

namespace noctalia::completions {
  namespace {

    using cli_schema::CliCommand;
    using cli_schema::CliFlag;
    using cli_schema::CliPositional;
    using cli_schema::ParsedArgs;

    constexpr const char* kHelpText = "Usage: noctalia completions [bash|fish|zsh]\n"
                                      "\n"
                                      "Print a shell completion script to stdout.\n"
                                      "\n"
                                      "Shells:\n"
                                      "  bash\n"
                                      "  zsh\n"
                                      "  fish\n"
                                      "\n"
                                      "Examples:\n"
                                      "  source <(noctalia completions bash)\n"
                                      "  noctalia completions zsh > \"${fpath[1]}/_noctalia\"\n"
                                      "  noctalia completions fish > ~/.config/fish/completions/noctalia.fish\n";

    constexpr std::string_view kShellChoices[] = {"bash", "fish", "zsh"};

    constexpr CliPositional kCompletionsPositionals[] = {
        {.name = "shell",
         .description = "target shell (bash, fish, zsh)",
         .choices = kShellChoices,
         .required = true,
         .missingError = "missing shell choice",
         .invalidChoiceError = "unknown shell"},
    };

    constexpr CliCommand kCompletionsCmd = {
        .name = "completions",
        .summary = "Generate shell completions",
        .helpText = kHelpText,
        .positionals = kCompletionsPositionals,
    };
  } // namespace

  int runCli(int argc, char* argv[]) {
    if (argc >= 3 && std::strcmp(argv[2], "--help") == 0) {
      std::println("{}", kHelpText);
      return argc < 3 ? 1 : 0;
    }

    const auto parsed = cli_schema::parseArgs(argc, argv, 2, kCompletionsCmd);
    if (!parsed) {
      std::println(stderr, "error: {}", parsed.error());
      std::println(stderr, "Run 'noctalia completions --help' for usage.");
      return 1;
    }

    if (parsed->helpRequested) {
      return 0;
    }

    const std::string_view shell = parsed->positionals[0];

    const char* script = nullptr;
    if (shell == "bash") {
      script = NOCTALIA_BASH_SCRIPT;
    } else if (shell == "zsh") {
      script = NOCTALIA_ZSH_SCRIPT;
    } else if (shell == "fish") {
      script = NOCTALIA_FISH_SCRIPT;
    }

    if (script != nullptr) {
      std::fwrite(script, 1, std::strlen(script), stdout);
    }

    return 0;
  }

} // namespace noctalia::completions
