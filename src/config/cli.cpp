#include "config/cli.h"

#include "config/config_service.h"
#include "config/config_validate.h"
#include "core/log.h"
#include "core/toml.h" // IWYU pragma: keep
#include "shell/settings/settings_registry.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace noctalia::config {
  namespace {

    constexpr const char* kHelpText =
        "Usage: noctalia config <command> [options]\n"
        "\n"
        "Commands:\n"
        "  validate [path]\n"
        "      Check config validity: TOML syntax, unknown/misspelled settings, and bad\n"
        "      values. Defaults to the active config dir + state settings.toml. A directory\n"
        "      validates its *.toml files; a file validates only that file. Exit 1 on error.\n"
        "\n"
        "  export [merged|full]\n"
        "      Print the active config as TOML. Defaults to merged user config.\n"
        "\n"
        "  settings-count\n"
        "      Count Settings UI controls by registry, visibility state, and section.\n"
        "\n"
        "  replay-report <report.toml> --target <dir> [--force]\n"
        "      Reconstruct config-home/noctalia and state-home/noctalia from a support report.\n"
        "\n"
        "  replay-report <report.toml> --target <dir> --flattened [--force]\n"
        "      Reconstruct a single config-home/noctalia/config.toml from the report's merged config.\n";

    constexpr const char* kValidateHelpText =
        "Usage: noctalia config validate [path]\n"
        "\n"
        "With no path, validates the merged configuration the way the shell loads it:\n"
        "  - every *.toml in the active config dir, then\n"
        "  - the state-dir settings.toml overrides.\n"
        "\n"
        "With a directory path, validates only that directory's *.toml files.\n"
        "With a file path, validates only that file.\n"
        "\n"
        "Reports TOML syntax errors, unknown sections/settings, and bad values\n"
        "(wrong type, out-of-range, invalid enum/color). Exits 1 if any error is found.\n";

    constexpr const char* kReplayHelpText =
        "Usage: noctalia config replay-report <report.toml> --target <dir> [--flattened] [--force]\n"
        "\n"
        "Options:\n"
        "  --target <dir>  Directory where replay files are written\n"
        "  --flattened     Write only merged_config.content as config.toml\n"
        "  --force         Remove an existing target directory before writing\n";

    constexpr const char* kExportHelpText = "Usage: noctalia config export [merged|full]\n"
                                            "\n"
                                            "Prints TOML to stdout from the same config stack used by the shell:\n"
                                            "  - every *.toml in the active config dir, then\n"
                                            "  - the state-dir settings.toml overrides.\n"
                                            "\n"
                                            "Modes:\n"
                                            "  merged  Export merged user config only (default)\n"
                                            "  full    Export full effective config, including built-in defaults\n";

    constexpr const char* kSettingsCountHelpText =
        "Usage: noctalia config settings-count\n"
        "\n"
        "Counts one Settings UI row/control per SettingEntry: toggles, sliders, lists,\n"
        "and pickers. Dropdown options and SettingsWindow-only action buttons are not\n"
        "counted separately.\n";

    struct ReplayOptions {
      std::filesystem::path reportPath;
      std::filesystem::path targetDir;
      bool flattened = false;
      bool force = false;
    };

    struct ReplayOptionsParse {
      ReplayOptions options;
      bool helpRequested = false;
    };

    struct SettingsCountSet {
      std::size_t total = 0;
      std::size_t visibleNormal = 0;
      std::size_t visibleAdvanced = 0;
    };

    bool passesVisibility(const Config& cfg, const settings::SettingEntry& entry) {
      return !entry.visibleWhen || entry.visibleWhen(cfg);
    }

    bool visibleWithAdvanced(const Config& cfg, const settings::SettingEntry& entry, bool showAdvanced) {
      return (showAdvanced || !entry.advanced) && passesVisibility(cfg, entry);
    }

    SettingsCountSet countSettingsEntries(const std::vector<settings::SettingEntry>& entries, const Config& cfg) {
      SettingsCountSet out;
      out.total = entries.size();
      for (const auto& entry : entries) {
        if (visibleWithAdvanced(cfg, entry, false)) {
          ++out.visibleNormal;
        }
        if (visibleWithAdvanced(cfg, entry, true)) {
          ++out.visibleAdvanced;
        }
      }
      return out;
    }

    std::size_t countAdvancedMarked(const std::vector<settings::SettingEntry>& entries) {
      return static_cast<std::size_t>(std::ranges::count(entries, true, &settings::SettingEntry::advanced));
    }

    std::size_t countConditionallyHidden(const std::vector<settings::SettingEntry>& entries, const Config& cfg) {
      return static_cast<std::size_t>(std::ranges::count_if(entries, [&](const settings::SettingEntry& entry) {
        return !passesVisibility(cfg, entry);
      }));
    }

    int runSettingsCount(int argc, char* argv[]) {
      for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0) {
          std::println("{}", kSettingsCountHelpText);
          return 0;
        }
        std::println(stderr, "error: unexpected argument: {}", argv[i]);
        std::println(stderr, "Run 'noctalia config settings-count --help' for usage.");
        return 1;
      }

      setLogLevel(LogLevel::Warn);
      ConfigService configService;
      const Config& cfg = configService.config();
      settings::RegistryEnvironment env;
      std::vector<settings::SettingEntry> registry = settings::buildSettingsRegistry(cfg, nullptr, nullptr, env);
      const SettingsCountSet registryCounts = countSettingsEntries(registry, cfg);

      std::println("Settings controls");
      std::println("Unit: one SettingEntry row/control. Dropdown options are not counted.");
      std::println("Runtime action buttons inserted by SettingsWindow are not counted.");

      std::println();
      std::println("Other totals");
      std::println("  total registry controls:       {}", registryCounts.total);
      std::println("  visible with Advanced off:     {}", registryCounts.visibleNormal);
      std::println("  visible with Advanced on:      {}", registryCounts.visibleAdvanced);
      std::println("  advanced-marked controls:      {}", countAdvancedMarked(registry));
      std::println("  conditionally hidden controls: {}", countConditionallyHidden(registry, cfg));
      std::println(
          "  visible only with Advanced on: {}", registryCounts.visibleAdvanced - registryCounts.visibleNormal
      );

      std::map<std::string, SettingsCountSet> sectionCounts;
      for (const auto& descriptor : settings::settingsSectionDescriptors()) {
        sectionCounts.emplace(std::string(descriptor.id), SettingsCountSet{});
      }
      for (const auto& entry : registry) {
        auto& counts = sectionCounts[std::string(settings::settingsSectionId(entry.section))];
        ++counts.total;
        if (visibleWithAdvanced(cfg, entry, false)) {
          ++counts.visibleNormal;
        }
        if (visibleWithAdvanced(cfg, entry, true)) {
          ++counts.visibleAdvanced;
        }
      }

      std::println();
      std::println("By section");
      std::println("  {:<14} {:>8} {:>8} {:>8}", "section", "total", "normal", "advanced");
      for (const auto& descriptor : settings::settingsSectionDescriptors()) {
        const auto it = sectionCounts.find(std::string(descriptor.id));
        if (it == sectionCounts.end() || it->second.total == 0) {
          continue;
        }
        std::println(
            "  {:<14} {:>8} {:>8} {:>8}", descriptor.id, it->second.total, it->second.visibleNormal,
            it->second.visibleAdvanced
        );
      }

      return 0;
    }

    std::expected<void, std::string> writeTextFile(const std::filesystem::path& path, std::string_view content) {
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      if (ec) {
        return std::unexpected("failed to create " + path.parent_path().string() + ": " + ec.message());
      }

      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      if (!out.is_open()) {
        return std::unexpected("failed to open " + path.string());
      }
      out.write(content.data(), static_cast<std::streamsize>(content.size()));
      if (!out.good()) {
        return std::unexpected("failed to write " + path.string());
      }
      return {};
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

    std::expected<void, std::string> prepareTarget(const std::filesystem::path& target, bool force) {
      std::error_code ec;
      if (std::filesystem::exists(target, ec) && !force) {
        return std::unexpected("target already exists; pass --force to replace it: " + target.string());
      }
      std::filesystem::create_directories(target, ec);
      if (ec) {
        return std::unexpected("failed to create target " + target.string() + ": " + ec.message());
      }
      return {};
    }

    std::expected<ReplayOptionsParse, std::string> parseReplayOptions(int argc, char* argv[]) {
      ReplayOptionsParse parsed;
      for (int i = 3; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--help") == 0) {
          std::println("{}", kReplayHelpText);
          parsed.helpRequested = true;
          return parsed;
        }
        if (std::strcmp(arg, "--target") == 0) {
          if (i + 1 >= argc) {
            return std::unexpected("--target requires a directory");
          }
          parsed.options.targetDir = argv[++i];
          continue;
        }
        if (std::strcmp(arg, "--flattened") == 0) {
          parsed.options.flattened = true;
          continue;
        }
        if (std::strcmp(arg, "--force") == 0) {
          parsed.options.force = true;
          continue;
        }
        if (parsed.options.reportPath.empty()) {
          parsed.options.reportPath = arg;
          continue;
        }
        return std::unexpected(std::string("unknown argument: ") + arg);
      }

      if (parsed.options.reportPath.empty()) {
        return std::unexpected("missing report path");
      }
      if (parsed.options.targetDir.empty()) {
        return std::unexpected("missing --target <dir>");
      }
      return parsed;
    }

    int replayReport(const ReplayOptions& options, const char* argv0) {
      toml::table report;
      try {
        report = toml::parse_file(options.reportPath.string());
      } catch (const toml::parse_error& e) {
        std::println(stderr, "error: failed to parse report: {}", e.what());
        return 1;
      }

      const std::filesystem::path target = std::filesystem::absolute(options.targetDir).lexically_normal();
      if (auto prepared = prepareTarget(target, options.force); !prepared) {
        std::println(stderr, "error: {}", prepared.error());
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
          std::println(stderr, "error: failed to remove {}: {}", configHome.string(), ec.message());
          return 1;
        }
        std::filesystem::remove_all(stateHome, ec);
        if (ec) {
          std::println(stderr, "error: failed to remove {}: {}", stateHome.string(), ec.message());
          return 1;
        }
      }

      if (options.flattened) {
        const auto merged = report["merged_config"]["content"].value<std::string>();
        if (!merged.has_value()) {
          std::println(stderr, "error: report has no [merged_config].content");
          return 1;
        }
        if (auto written = writeTextFile(configDir / "config.toml", *merged); !written) {
          std::println(stderr, "error: {}", written.error());
          return 1;
        }
        std::error_code ec;
        std::filesystem::create_directories(stateDir, ec);
        if (ec) {
          std::println(stderr, "error: failed to create {}: {}", stateDir.string(), ec.message());
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
              std::println(stderr, "error: report contains an unsafe config source path");
              return 1;
            }
            if (auto written = writeTextFile(configDir / *relative, *content); !written) {
              std::println(stderr, "error: {}", written.error());
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
          if (auto written = writeTextFile(stateDir / "settings.toml", content); !written) {
            std::println(stderr, "error: {}", written.error());
            return 1;
          }
        } else {
          std::error_code ec;
          std::filesystem::create_directories(stateDir, ec);
          if (ec) {
            std::println(stderr, "error: failed to create {}: {}", stateDir.string(), ec.message());
            return 1;
          }
        }

        const auto* appState = report["app_state"].as_table();
        bool appStateExists = appState != nullptr;
        if (appState != nullptr) {
          if (auto exists = (*appState)["exists"].value<bool>()) {
            appStateExists = *exists;
          }
        }
        if (appStateExists && appState != nullptr) {
          const auto content = (*appState)["content"].value<std::string>().value_or("");
          if (auto written = writeTextFile(stateDir / "state.toml", content); !written) {
            std::println(stderr, "error: {}", written.error());
            return 1;
          }
        }
      }

      std::println("Replayed support report into {}", target.string());
      std::println();
      std::println("Config home: {}", configHome.string());
      std::println("State home:  {}", stateHome.string());
      std::println();
      std::println("Run with:");
      std::println(
          "  NOCTALIA_CONFIG_HOME={} NOCTALIA_STATE_HOME={} {}", StringUtils::shellQuote(configHome.string()),
          StringUtils::shellQuote(stateHome.string()), StringUtils::shellQuote(argv0)
      );
      return 0;
    }

    // ANSI color only when the stream is a terminal and NO_COLOR is unset, so
    // piped/redirected output stays clean.
    bool useColor(std::FILE* stream) {
      static const bool noColor = std::getenv("NO_COLOR") != nullptr;
      return !noColor && isatty(fileno(stream)) != 0;
    }

    int runValidate(int argc, char* argv[]) {
      std::string pathArg;
      for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0) {
          std::println("{}", kValidateHelpText);
          return 0;
        }
        if (pathArg.empty()) {
          pathArg = argv[i];
          continue;
        }
        std::println(stderr, "error: unexpected argument: {}", argv[i]);
        std::println(stderr, "Run 'noctalia config validate --help' for usage.");
        return 1;
      }

      // Validation reports through diagnostics below; silence incidental INFO logs
      // (e.g. the plugin registry scan) so only validation results reach the user.
      setLogLevel(LogLevel::Warn);
      schema::Diagnostics diagnostics;

      if (pathArg.empty()) {
        const std::string configDir = FileUtils::configDir();
        std::string settingsPath;
        if (const std::string stateDir = FileUtils::stateDir(); !stateDir.empty()) {
          settingsPath = stateDir + "/settings.toml";
        }
        diagnostics = validateConfigSources(configDir, settingsPath);
      } else {
        std::error_code ec;
        const std::filesystem::path inputPath(pathArg);
        const auto status = std::filesystem::status(inputPath, ec);
        if (ec) {
          std::println(stderr, "error: failed to inspect {}: {}", pathArg, ec.message());
          return 1;
        }
        if (std::filesystem::is_directory(status)) {
          diagnostics = validateConfigSources(pathArg, {});
        } else if (std::filesystem::is_regular_file(status)) {
          diagnostics = validateConfigFile(pathArg);
        } else {
          std::println(stderr, "error: path is not a regular file or directory: {}", pathArg);
          return 1;
        }
      }

      const bool colorErr = useColor(stderr);
      const bool colorOut = useColor(stdout);

      std::size_t errors = 0;
      std::size_t warnings = 0;
      for (const auto& entry : diagnostics.entries) {
        const bool isError = entry.severity == schema::Diagnostics::Severity::Error;
        (isError ? errors : warnings)++;
        std::FILE* out = isError ? stderr : stdout;
        const char* tag = isError ? "ERROR" : "WARN "; // padded to align the path column
        const char* color = (isError ? colorErr : colorOut) ? (isError ? "\033[31m" : "\033[33m") : "";
        const char* reset = *color != '\0' ? "\033[0m" : "";
        std::println(out, "{}{}{} {}: {}", color, tag, reset, entry.path, entry.message);
      }

      if (errors > 0) {
        const char* c = colorErr ? "\033[31m" : "";
        const char* r = colorErr ? "\033[0m" : "";
        std::println(stderr);
        std::println(stderr, "{}✗ Config is invalid{} ({} error(s), {} warning(s))", c, r, errors, warnings);
        return 1;
      }
      const char* c = colorOut ? "\033[32m" : "";
      const char* r = colorOut ? "\033[0m" : "";
      if (warnings > 0) {
        std::println();
        std::println("{}✓ Config is valid{} ({} warning(s))", c, r, warnings);
      } else {
        std::println("{}✓ Config is valid{}", c, r);
      }
      return 0;
    }

    int runExport(int argc, char* argv[]) {
      std::string mode = "merged";
      bool modeSet = false;
      for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0) {
          std::println("{}", kExportHelpText);
          return 0;
        }
        if (!modeSet) {
          mode = argv[i];
          modeSet = true;
          continue;
        }
        std::println(stderr, "error: unexpected argument: {}", argv[i]);
        std::println(stderr, "Run 'noctalia config export --help' for usage.");
        return 1;
      }

      const std::string configDir = FileUtils::configDir();
      std::string settingsPath;
      if (const std::string stateDir = FileUtils::stateDir(); !stateDir.empty()) {
        settingsPath = stateDir + "/settings.toml";
      }

      std::string error;
      std::string content;
      if (mode == "merged") {
        content = ConfigService::buildMergedUserConfigFromSources(configDir, settingsPath, &error);
      } else if (mode == "full") {
        content = ConfigService::buildEffectiveConfigFromSources(configDir, settingsPath, &error);
      } else {
        std::println(stderr, "error: expected merged or full");
        return 1;
      }

      if (!error.empty()) {
        std::println(stderr, "error: {}", error);
        return 1;
      }

      std::fputs(content.c_str(), stdout);
      return 0;
    }

  } // namespace

  int runCli(int argc, char* argv[]) {
    if (argc < 3 || std::strcmp(argv[2], "--help") == 0) {
      std::println("{}", kHelpText);
      return argc < 3 ? 1 : 0;
    }

    if (std::strcmp(argv[2], "validate") == 0) {
      return runValidate(argc, argv);
    }

    if (std::strcmp(argv[2], "export") == 0) {
      return runExport(argc, argv);
    }

    if (std::strcmp(argv[2], "settings-count") == 0) {
      return runSettingsCount(argc, argv);
    }

    if (std::strcmp(argv[2], "replay-report") == 0) {
      const auto parsed = parseReplayOptions(argc, argv);
      if (!parsed) {
        std::println(stderr, "error: {}", parsed.error());
        std::println(stderr, "Run 'noctalia config replay-report --help' for usage.");
        return 1;
      }
      if (parsed->helpRequested) {
        return 0;
      }
      return replayReport(parsed->options, argv[0]);
    }

    std::println(stderr, "error: unknown config command: {}", argv[2]);
    std::println(stderr, "Run 'noctalia config --help' for usage.");
    return 1;
  }

} // namespace noctalia::config
