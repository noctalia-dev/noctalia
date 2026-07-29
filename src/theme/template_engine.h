#pragma once

#include "core/toml.h" // IWYU pragma: keep

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace noctalia::theme {

  struct GeneratedPalette;

  struct RenderResult {
    std::string text;
    int errorCount = 0;
  };

  struct RenderFileResult {
    bool success = false;
    bool wrote = false;
    int errorCount = 0;
  };

  class TemplateEngine {
  public:
    using ModeMap = std::unordered_map<std::string, std::string>;
    using ThemeData = std::unordered_map<std::string, ModeMap>;

    struct Options {
      std::string defaultMode = "dark";
      std::string imagePath;
      std::string closestColor;
      std::string configDir;
      std::string configFile;
      std::unordered_set<std::string> enabledTemplates;
      std::function<bool()> cancelRequested;
      std::string schemeType = "content";
      bool verbose = true;
    };

    explicit TemplateEngine(ThemeData themeData);
    TemplateEngine(ThemeData themeData, Options options);

    static ThemeData makeThemeData(const GeneratedPalette& palette);

    RenderResult render(std::string_view templateText);
    RenderFileResult renderFile(const std::filesystem::path& inputPath, const std::filesystem::path& outputPath);
    bool processConfigFile(const std::filesystem::path& configPath);
    bool processConfigTable(const toml::table& root, const std::filesystem::path& configPath);

    // The two halves of processConfigTable(), for callers that must ingest [config.custom_colors]
    // before rendering templates that are not declared in this config.
    void applyCustomColors(const toml::table& root);
    bool processConfigTemplates(const toml::table& root, const std::filesystem::path& configPath);

  private:
    ThemeData m_themeData;
    Options m_options;
  };

} // namespace noctalia::theme
