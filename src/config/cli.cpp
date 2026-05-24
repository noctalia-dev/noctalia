#include "config/cli.h"

#include "core/toml.h" // IWYU pragma: keep
#include "util/string_utils.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace noctalia::config {
  namespace {

    constexpr const char* kHelpText =
        "Usage: noctalia config <command> [options]\n"
        "\n"
        "Commands:\n"
        "  replay-report <report.toml> --target <dir> [--force]\n"
        "      Reconstruct config-home/noctalia and state-home/noctalia from a support report.\n"
        "\n"
        "  replay-report <report.toml> --target <dir> --flattened [--force]\n"
        "      Reconstruct a single config-home/noctalia/config.toml from the report's merged config.\n";

    constexpr const char* kReplayHelpText =
        "Usage: noctalia config replay-report <report.toml> --target <dir> [--flattened] [--force]\n"
        "\n"
        "Options:\n"
        "  --target <dir>  Directory where replay files are written\n"
        "  --flattened     Write only merged_config.content as config.toml\n"
        "  --force         Remove an existing target directory before writing\n";

    struct ReplayOptions {
      std::filesystem::path reportPath;
      std::filesystem::path targetDir;
      bool flattened = false;
      bool force = false;
    };

    bool writeTextFile(const std::filesystem::path& path, std::string_view content, std::string& error) {
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      if (ec) {
        error = "failed to create " + path.parent_path().string() + ": " + ec.message();
        return false;
      }

      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      if (!out.is_open()) {
        error = "failed to open " + path.string();
        return false;
      }
      out.write(content.data(), static_cast<std::streamsize>(content.size()));
      if (!out.good()) {
        error = "failed to write " + path.string();
        return false;
      }
      return true;
    }

    std::optional<std::filesystem::path> safeRelativePath(const toml::table& table, std::string_view fallback) {
      std::string raw;
      if (auto value = table["relative_path"].value<std::string>()) {
        raw = *value;
      } else {
        raw = std::string(fallback);
      }
      if (raw.empty()) {
        return std::nullopt;
      }

      std::filesystem::path path(raw);
      if (path.is_absolute()) {
        return std::nullopt;
      }
      for (const auto& part : path) {
        if (part == "..") {
          return std::nullopt;
        }
      }
      return path.lexically_normal();
    }

    bool prepareTarget(const std::filesystem::path& target, bool force, std::string& error) {
      std::error_code ec;
      if (std::filesystem::exists(target, ec) && !force) {
        error = "target already exists; pass --force to replace it: " + target.string();
        return false;
      }
      std::filesystem::create_directories(target, ec);
      if (ec) {
        error = "failed to create target " + target.string() + ": " + ec.message();
        return false;
      }
      return true;
    }

    std::optional<ReplayOptions> parseReplayOptions(int argc, char* argv[], std::string& error) {
      ReplayOptions options;
      for (int i = 3; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--help") == 0) {
          std::puts(kReplayHelpText);
          return std::nullopt;
        }
        if (std::strcmp(arg, "--target") == 0) {
          if (i + 1 >= argc) {
            error = "--target requires a directory";
            return std::nullopt;
          }
          options.targetDir = argv[++i];
          continue;
        }
        if (std::strcmp(arg, "--flattened") == 0) {
          options.flattened = true;
          continue;
        }
        if (std::strcmp(arg, "--force") == 0) {
          options.force = true;
          continue;
        }
        if (options.reportPath.empty()) {
          options.reportPath = arg;
          continue;
        }
        error = std::string("unknown argument: ") + arg;
        return std::nullopt;
      }

      if (options.reportPath.empty()) {
        error = "missing report path";
        return std::nullopt;
      }
      if (options.targetDir.empty()) {
        error = "missing --target <dir>";
        return std::nullopt;
      }
      return options;
    }

    int replayReport(const ReplayOptions& options, const char* argv0) {
      toml::table report;
      try {
        report = toml::parse_file(options.reportPath.string());
      } catch (const toml::parse_error& e) {
        std::fprintf(stderr, "error: failed to parse report: %s\n", e.what());
        return 1;
      }

      const std::filesystem::path target = std::filesystem::absolute(options.targetDir).lexically_normal();
      std::string error;
      if (!prepareTarget(target, options.force, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
      }

      const std::filesystem::path configHome = target / "config-home";
      const std::filesystem::path stateHome = target / "state-home";
      const std::filesystem::path configDir = configHome / "noctalia";
      const std::filesystem::path stateDir = stateHome / "noctalia";

      if (options.force) {
        std::error_code ec;
        std::filesystem::remove_all(configHome, ec);
        if (ec) {
          std::fprintf(stderr, "error: failed to remove %s: %s\n", configHome.string().c_str(), ec.message().c_str());
          return 1;
        }
        std::filesystem::remove_all(stateHome, ec);
        if (ec) {
          std::fprintf(stderr, "error: failed to remove %s: %s\n", stateHome.string().c_str(), ec.message().c_str());
          return 1;
        }
      }

      if (options.flattened) {
        const auto merged = report["merged_config"]["content"].value<std::string>();
        if (!merged.has_value()) {
          std::fputs("error: report has no [merged_config].content\n", stderr);
          return 1;
        }
        if (!writeTextFile(configDir / "config.toml", *merged, error)) {
          std::fprintf(stderr, "error: %s\n", error.c_str());
          return 1;
        }
        std::error_code ec;
        std::filesystem::create_directories(stateDir, ec);
        if (ec) {
          std::fprintf(stderr, "error: failed to create %s: %s\n", stateDir.string().c_str(), ec.message().c_str());
          return 1;
        }
      } else {
        const auto* sources = report["config_sources"].as_array();
        if (sources != nullptr) {
          std::size_t fallbackIndex = 0;
          for (const auto& sourceNode : *sources) {
            const auto* source = sourceNode.as_table();
            if (source == nullptr) {
              continue;
            }
            const auto content = (*source)["content"].value<std::string>();
            if (!content.has_value()) {
              continue;
            }

            const auto relative = safeRelativePath(*source, "config_" + std::to_string(fallbackIndex++) + ".toml");
            if (!relative.has_value()) {
              std::fputs("error: report contains an unsafe config source path\n", stderr);
              return 1;
            }
            if (!writeTextFile(configDir / *relative, *content, error)) {
              std::fprintf(stderr, "error: %s\n", error.c_str());
              return 1;
            }
          }
        }

        const auto* state = report["state_settings"].as_table();
        bool stateExists = state != nullptr;
        if (state != nullptr) {
          if (auto exists = (*state)["exists"].value<bool>()) {
            stateExists = *exists;
          }
        }
        if (stateExists && state != nullptr) {
          const auto content = (*state)["content"].value<std::string>().value_or("");
          if (!writeTextFile(stateDir / "settings.toml", content, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
          }
        } else {
          std::error_code ec;
          std::filesystem::create_directories(stateDir, ec);
          if (ec) {
            std::fprintf(stderr, "error: failed to create %s: %s\n", stateDir.string().c_str(), ec.message().c_str());
            return 1;
          }
        }
      }

      std::printf("Replayed support report into %s\n\n", target.string().c_str());
      std::printf("Config home: %s\n", configHome.string().c_str());
      std::printf("State home:  %s\n\n", stateHome.string().c_str());
      std::printf("Run with:\n");
      std::printf(
          "  NOCTALIA_CONFIG_HOME=%s NOCTALIA_STATE_HOME=%s %s\n", StringUtils::shellQuote(configHome.string()).c_str(),
          StringUtils::shellQuote(stateHome.string()).c_str(), StringUtils::shellQuote(argv0).c_str()
      );
      return 0;
    }

  } // namespace

  int runCli(int argc, char* argv[]) {
    if (argc < 3 || std::strcmp(argv[2], "--help") == 0) {
      std::puts(kHelpText);
      return argc < 3 ? 1 : 0;
    }

    if (std::strcmp(argv[2], "replay-report") == 0) {
      std::string error;
      const auto options = parseReplayOptions(argc, argv, error);
      if (!options.has_value()) {
        if (!error.empty()) {
          std::fprintf(stderr, "error: %s\n", error.c_str());
          std::fputs("Run 'noctalia config replay-report --help' for usage.\n", stderr);
          return 1;
        }
        return 0;
      }
      return replayReport(*options, argv[0]);
    }

    std::fprintf(stderr, "error: unknown config command: %s\n", argv[2]);
    std::fputs("Run 'noctalia config --help' for usage.\n", stderr);
    return 1;
  }

} // namespace noctalia::config
