
#include "cli/parse.h"
<<<<<<< HEAD
=======
#include "cli/schema_root.h"
#include "completions/generator.h"
>>>>>>> f9622623c (refactor: header-only cli schema)

#include <print>
#include <string_view>

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
<<<<<<< HEAD
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
=======

  int runCli(int argc, char* argv[]) {
    if (argc >= 3 && (std::string_view(argv[2]) == "--help" || std::string_view(argv[2]) == "-h")) {
      std::println("Usage: noctalia completions [bash|fish|zsh]\n");
      std::println("Print shell completion script dynamically generated from CLI schema.");
>>>>>>> f9622623c (refactor: header-only cli schema)
      return 0;
    }

    if (argc < 3) {
      std::println(stderr, "error: missing shell argument (expected bash, fish, or zsh)");
      std::println(stderr, "Run 'noctalia completions --help' for usage.");
      return 1;
    }

    const std::string_view shell = argv[2];
    std::string script;

    if (shell == "fish") {
      script = generateFish(cli::kRootCmd);
    } else if (shell == "zsh") {
      script = generateZsh(cli::kRootCmd);
    } else if (shell == "bash") {
      script = generateBash(cli::kRootCmd);
    } else {
      std::println(stderr, "error: unknown shell '{}' (expected bash, fish, or zsh)", shell);
      return 1;
    }

<<<<<<< HEAD
    if (script != nullptr) {
      std::fwrite(script, 1, std::strlen(script), stdout);
    }

=======
    std::print("{}", script);
>>>>>>> f9622623c (refactor: header-only cli schema)
    return 0;
  }

} // namespace noctalia::completions
