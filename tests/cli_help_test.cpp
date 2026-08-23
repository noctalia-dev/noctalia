#include "cli/help.h"
#include "tests/test_check.h"

#include <array>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

  constexpr std::array kAlphaPositionals{
      noctalia::cli::Positional{"path", {}, {}, false, false, false},
  };
  constexpr std::array kChildren{
      noctalia::cli::Command{"zeta", "Run zeta", {}, {}, {}, {}, {}, false},
      noctalia::cli::Command{"alpha", "Run alpha", {}, {}, {}, kAlphaPositionals, {}, false},
  };
  constexpr std::array kGroupFlags{
      noctalia::cli::Flag{"--verbose", {}, {}, "Enable verbose output", {}, {}, false, false},
  };
  constexpr noctalia::cli::Command kGroup{
      "tools", "Tool commands", "Manage offline tools.", {}, kGroupFlags, {}, kChildren, false,
  };

  constexpr std::array<std::string_view, 2> kKinds{"full", "merged"};
  constexpr std::array kLeafPositionals{
      noctalia::cli::Positional{"input", "Input file", {}, true, false, false},
      noctalia::cli::Positional{"kind", {}, kKinds, false, false, false},
  };
  constexpr std::array kLeafFlags{
      noctalia::cli::Flag{{}, "-o", "<file>", "Write output", {}, {}, false, false},
      noctalia::cli::Flag{"--mode", "-m", "<mode>", "Select mode", kKinds, "full", false, false},
      noctalia::cli::Flag{"--force", {}, {}, "Overwrite output", {}, {}, false, false},
  };
  constexpr noctalia::cli::Command kLeaf{
      "convert",
      "Convert input",
      "Convert a source file.\n\nThe output format is selected independently.",
      "See https://example.test for details.",
      kLeafFlags,
      kLeafPositionals,
      {},
      false,
  };

  void checkGroupHelp() {
    const std::string expected = "Usage: noctalia tools <command> [options]\n"
                                 "\n"
                                 "Manage offline tools.\n"
                                 "\n"
                                 "Commands:\n"
                                 "  alpha [path]  Run alpha\n"
                                 "  zeta          Run zeta\n"
                                 "\n"
                                 "Options:\n"
                                 "      --verbose  Enable verbose output\n"
                                 "  -h, --help     Show this help message\n";
    TEST_CHECK(noctalia::cli::renderHelp(kGroup, "noctalia tools") == expected);
  }

  void checkLeafHelp() {
    const std::string expected = "Usage: noctalia convert <input> [kind] [options]\n"
                                 "\n"
                                 "Convert a source file.\n"
                                 "\n"
                                 "The output format is selected independently.\n"
                                 "\n"
                                 "Arguments:\n"
                                 "  <input>  Input file\n"
                                 "  [kind]   one of: full, merged\n"
                                 "\n"
                                 "Options:\n"
                                 "  -o <file>          Write output\n"
                                 "  -m, --mode <mode>  Select mode (default: full)\n"
                                 "      --force        Overwrite output\n"
                                 "  -h, --help         Show this help message\n"
                                 "\n"
                                 "See https://example.test for details.\n";
    TEST_CHECK(noctalia::cli::renderHelp(kLeaf, "noctalia convert") == expected);
    TEST_CHECK(noctalia::cli::renderArgsSpec(kLeaf) == "<input> [kind] [options]");
    TEST_CHECK(noctalia::cli::renderArgsSpec(kGroup) == "<{zeta|alpha}> ...");
  }

} // namespace

int main() {
  static_assert(noctalia::cli::validateCommand(kGroup));
  static_assert(noctalia::cli::validateCommand(kLeaf));
  checkGroupHelp();
  checkLeafHelp();
  return EXIT_SUCCESS;
}
