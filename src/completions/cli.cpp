#include "completions/cli.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <string>
#include <unistd.h>

namespace noctalia::completions {
  namespace {

    constexpr const char* kHelpText = "Usage: noctalia completions <shell>\n"
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
                                      "  noctalia completions fish > ~/.config/fish/completions/noctalia.fish\n";

    [[nodiscard]] std::string selfExePath() {
      char buf[4096];
      const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
      if (n <= 0) {
        return {};
      }
      buf[n] = '\0';
      return std::string(buf, static_cast<std::size_t>(n));
    }

    [[nodiscard]] std::filesystem::path completionsAssetDir() {
      // Forced ENV
      if (const char* env_dir = std::getenv("NOCTALIA_ASSETS_DIR")) {
        const auto envPath = std::filesystem::path(env_dir) / "completions";
        std::error_code ec;
        if (std::filesystem::is_directory(envPath, ec)) {
          return envPath;
        }
      }

#ifdef NOCTALIA_SOURCE_ASSETS_DIR
      {
        const auto source = std::filesystem::path(NOCTALIA_SOURCE_ASSETS_DIR) / "completions";
        std::error_code ec;
        if (std::filesystem::is_directory(source, ec)) {
          return source;
        }
      }
#endif

#if defined(NOCTALIA_INSTALL_PREFIX) && defined(NOCTALIA_INSTALL_DATADIR)
      {
        const std::filesystem::path datadir(NOCTALIA_INSTALL_DATADIR);
        const auto installed = datadir.is_absolute()
            ? datadir / "noctalia" / "assets" / "completions"
            : std::filesystem::path(NOCTALIA_INSTALL_PREFIX) / datadir / "noctalia" / "assets" / "completions";
        std::error_code ec;
        if (std::filesystem::is_directory(installed, ec)) {
          return installed;
        }
      }
#endif

      // Fallback
      const std::string self = selfExePath();
      if (!self.empty()) {
        const auto candidate =
            std::filesystem::path(self).parent_path().parent_path() / "share" / "noctalia" / "assets" / "completions";
        std::error_code ec;
        if (std::filesystem::is_directory(candidate, ec)) {
          return candidate;
        }
      }
      return {};
    }

  } // namespace

  int runCli(int argc, char* argv[]) {
    if (argc < 3 || std::strcmp(argv[2], "--help") == 0) {
      std::println("{}", kHelpText);
      return argc < 3 ? 1 : 0;
    }

    if (argc > 3) {
      std::println(stderr, "error: unexpected argument: {}", argv[3]);
      std::println(stderr, "Run 'noctalia completions --help' for usage.");
      return 1;
    }

    const char* shell = argv[2];
    std::string filename;

    if (std::strcmp(shell, "bash") == 0) {
      filename = "noctalia.bash";
    } else if (std::strcmp(shell, "zsh") == 0) {
      filename = "_noctalia";
    } else if (std::strcmp(shell, "fish") == 0) {
      filename = "noctalia.fish";
    } else {
      std::println(stderr, "error: unknown shell: {}", shell);
      std::println(stderr, "Run 'noctalia completions --help' for usage.");
      return 1;
    }

    const auto assetDir = completionsAssetDir();
    if (assetDir.empty()) {
      std::println(stderr, "error: could not locate shipped completion assets");
      return 1;
    }

    const std::filesystem::path filepath = assetDir / filename;
    std::ifstream file(filepath);

    if (!file.is_open()) {
      std::println(stderr, "error: could not find completion script at {}", filepath.string());
      return 1;
    }

    std::cout << file.rdbuf();
    return 0;
  }

} // namespace noctalia::completions
