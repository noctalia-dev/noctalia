#include "theme/cli.h"

#include "config/config_service.h"
#include "core/resource_paths.h"
#include "core/toml.h" // IWYU pragma: keep
#include "theme/builtin_templates.h"
#include "theme/color.h"
#include "theme/community_templates.h"
#include "theme/fixed_palette.h"
#include "theme/image_loader.h"
#include "theme/json_output.h"
#include "theme/palette_generator.h"
#include "theme/scheme.h"
#include "theme/template_engine.h"
#include "util/file_utils.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace noctalia::theme {

  namespace {

    constexpr const char* kHelpText =
        "Usage: noctalia theme <image> [options]\n"
        "       noctalia theme --list-templates [-c <file>]\n"
        "\n"
        "Generate a color palette from an image. Material You and custom\n"
        "schemes produce very different results.\n"
        "\n"
        "Options:\n"
        "  --scheme <name>   Material You (Material Design 3):\n"
        "                      m3-tonal-spot  (default)\n"
        "                      m3-content\n"
        "                      m3-fruit-salad\n"
        "                      m3-rainbow\n"
        "                      m3-monochrome\n"
        "                    Custom (HSL-space, non-M3):\n"
        "                      vibrant\n"
        "                      faithful\n"
        "                      dysfunctional\n"
        "                      muted\n"
        "  --dark            Emit only the dark variant (default)\n"
        "  --light           Emit only the light variant\n"
        "  --both            Emit both variants under dark/light keys\n"
        "  --theme-json <f>  Load precomputed dark/light token maps from JSON\n"
        "  -o <file>         Write JSON to file instead of stdout\n"
        "  -r <in:out>       Render a template file to an output path\n"
        "  -c <file>         Process a TOML template config file\n"
        "  --builtin-config  Process the shipped built-in template catalog\n"
        "  --list-templates  List built-in, cached community, and configured user templates\n"
        "                    Use -c <file> to include a specific template config\n"
        "  --default-mode    Template default mode: dark or light";

    std::filesystem::path builtinTemplateConfigPath() { return paths::assetPath("templates/builtin.toml"); }

    using TokenMap = std::unordered_map<std::string, uint32_t>;

    struct TemplateListEntry {
      std::string id;
      std::string category;
      std::string name;
    };

    std::string templateNameOrId(const std::string& id, const std::string& name) { return name.empty() ? id : name; }

    void sortTemplateList(std::vector<TemplateListEntry>& entries) {
      std::sort(entries.begin(), entries.end(), [](const TemplateListEntry& lhs, const TemplateListEntry& rhs) {
        return std::tie(lhs.category, lhs.id, lhs.name) < std::tie(rhs.category, rhs.id, rhs.name);
      });
    }

    std::vector<TemplateListEntry> loadBuiltinTemplateList(std::string& err) {
      std::vector<TemplateListEntry> out;
      const auto builtins = noctalia::theme::loadBuiltinTemplateInfo(&err);
      out.reserve(builtins.size());
      for (const auto& builtin : builtins) {
        out.push_back(
            TemplateListEntry{
                .id = builtin.id,
                .category = builtin.category,
                .name = templateNameOrId(builtin.id, builtin.name),
            }
        );
      }
      sortTemplateList(out);
      return out;
    }

    std::vector<TemplateListEntry> loadCommunityTemplateList() {
      std::vector<TemplateListEntry> out;
      const auto community = CommunityTemplateService::availableTemplates();
      out.reserve(community.size());
      for (const auto& entry : community) {
        out.push_back(
            TemplateListEntry{
                .id = entry.id,
                .category = entry.category,
                .name = templateNameOrId(entry.id, entry.displayName),
            }
        );
      }
      sortTemplateList(out);
      return out;
    }

    std::vector<TemplateListEntry> loadConfiguredUserTemplateList() {
      ConfigService config;
      const auto& userTemplates = config.config().theme.templates.userTemplates;

      std::vector<TemplateListEntry> out;
      out.reserve(userTemplates.size());
      for (const auto& entry : userTemplates) {
        out.push_back(
            TemplateListEntry{
                .id = entry.id,
                .category = "user",
                .name = entry.id,
            }
        );
      }
      sortTemplateList(out);
      return out;
    }

    std::unordered_map<std::string, TemplateListEntry> loadTemplateCatalog(const toml::table& root) {
      std::unordered_map<std::string, TemplateListEntry> out;
      const toml::table* catalog = root["catalog"].as_table();
      if (catalog == nullptr)
        return out;

      for (const auto& [idNode, node] : *catalog) {
        const auto id = std::string(idNode.str());
        TemplateListEntry entry{.id = id, .category = {}, .name = id};
        if (const toml::table* info = node.as_table()) {
          if (const auto name = info->get_as<std::string>("name"))
            entry.name = name->get();
          if (const auto category = info->get_as<std::string>("category"))
            entry.category = category->get();
        }
        out[id] = std::move(entry);
      }
      return out;
    }

    std::vector<TemplateListEntry>
    loadTemplateConfigList(const std::filesystem::path& path, bool required, std::string& err) {
      std::error_code ec;
      if (!std::filesystem::exists(path, ec)) {
        if (required)
          err = "file does not exist";
        return {};
      }

      toml::table root;
      try {
        root = toml::parse_file(path.string());
      } catch (const toml::parse_error& e) {
        err = e.description();
        return {};
      }

      std::vector<TemplateListEntry> out;
      const toml::table* templates = root["templates"].as_table();
      if (templates == nullptr)
        return out;

      const auto catalog = loadTemplateCatalog(root);
      out.reserve(templates->size());
      for (const auto& [idNode, node] : *templates) {
        if (node.as_table() == nullptr)
          continue;
        const auto id = std::string(idNode.str());
        auto catalogIt = catalog.find(id);
        if (catalogIt != catalog.end()) {
          out.push_back(catalogIt->second);
        } else {
          out.push_back(TemplateListEntry{.id = id, .category = {}, .name = id});
        }
      }
      sortTemplateList(out);
      return out;
    }

    void printTemplateListGroup(const char* title, const std::vector<TemplateListEntry>& entries, bool& firstGroup) {
      if (entries.empty())
        return;

      if (!firstGroup)
        std::putchar('\n');
      firstGroup = false;
      std::printf("%s\n", title);

      std::size_t idWidth = std::strlen("ID");
      std::size_t categoryWidth = std::strlen("Category");
      for (const auto& entry : entries) {
        idWidth = std::max(idWidth, entry.id.size());
        categoryWidth = std::max(categoryWidth, entry.category.empty() ? std::size_t{1} : entry.category.size());
      }

      const auto idColumn = static_cast<int>(idWidth);
      const auto categoryColumn = static_cast<int>(categoryWidth);
      std::printf("  %-*s  %-*s  %s\n", idColumn, "ID", categoryColumn, "Category", "Name");
      for (const auto& entry : entries) {
        const std::string category = entry.category.empty() ? "-" : entry.category;
        std::printf(
            "  %-*s  %-*s  %s\n", idColumn, entry.id.c_str(), categoryColumn, category.c_str(), entry.name.c_str()
        );
      }
    }

    int listTemplates(const char* configPath) {
      std::string err;
      const auto builtins = loadBuiltinTemplateList(err);
      if (!err.empty()) {
        std::fprintf(stderr, "error: failed to load built-in templates: %s\n", err.c_str());
        return 1;
      }

      const auto community = loadCommunityTemplateList();
      std::vector<TemplateListEntry> userTemplates;
      std::string explicitConfigTitle = "Template config";
      if (configPath != nullptr) {
        const std::filesystem::path templateConfigPath = FileUtils::expandUserPath(configPath);
        std::string userErr;
        userTemplates = loadTemplateConfigList(templateConfigPath, true, userErr);
        if (!userErr.empty()) {
          std::fprintf(
              stderr, "error: failed to load template config %s: %s\n", templateConfigPath.string().c_str(),
              userErr.c_str()
          );
          return 1;
        }
        explicitConfigTitle += " (";
        explicitConfigTitle += templateConfigPath.filename().string();
        explicitConfigTitle += ")";
      } else {
        userTemplates = loadConfiguredUserTemplateList();
      }

      bool firstGroup = true;
      printTemplateListGroup("Built-in templates", builtins, firstGroup);
      printTemplateListGroup("Community templates (cached)", community, firstGroup);
      printTemplateListGroup(
          configPath != nullptr ? explicitConfigTitle.c_str() : "User templates", userTemplates, firstGroup
      );
      if (firstGroup)
        std::puts("No templates found.");
      return 0;
    }

    std::optional<Color> loadHexColor(const nlohmann::json& src, const char* key) {
      if (!src.contains(key) || !src[key].is_string())
        return std::nullopt;
      try {
        return Color::fromHex(src[key].get<std::string>());
      } catch (...) {
        return std::nullopt;
      }
    }

    void setToken(TokenMap& dst, std::string_view key, std::string_view hex) {
      dst[std::string(key)] = Color::fromHex(hex).toArgb();
    }

    std::optional<::Palette> parseFixedPaletteJson(const nlohmann::json& src, std::string& err) {
      const auto primary = loadHexColor(src, "mPrimary");
      const auto onPrimary = loadHexColor(src, "mOnPrimary");
      const auto secondary = loadHexColor(src, "mSecondary");
      const auto onSecondary = loadHexColor(src, "mOnSecondary");
      const auto tertiary = loadHexColor(src, "mTertiary");
      const auto onTertiary = loadHexColor(src, "mOnTertiary");
      const auto error = loadHexColor(src, "mError");
      const auto onError = loadHexColor(src, "mOnError");
      const auto surface = loadHexColor(src, "mSurface");
      const auto onSurface = loadHexColor(src, "mOnSurface");
      const auto surfaceVariant = loadHexColor(src, "mSurfaceVariant");
      const auto onSurfaceVariant = loadHexColor(src, "mOnSurfaceVariant");
      const auto outlineRaw = loadHexColor(src, "mOutline");
      const auto shadow = loadHexColor(src, "mShadow").value_or(surface.value_or(Color{}));

      if (!primary || !onPrimary || !secondary || !onSecondary || !tertiary || !onTertiary || !error || !onError ||
          !surface || !onSurface || !surfaceVariant || !onSurfaceVariant || !outlineRaw) {
        err = "fixed palette json is missing required colors";
        return std::nullopt;
      }
      return ::Palette{
          .primary = rgbHex(primary->toArgb() & 0x00FFFFFFU),
          .onPrimary = rgbHex(onPrimary->toArgb() & 0x00FFFFFFU),
          .secondary = rgbHex(secondary->toArgb() & 0x00FFFFFFU),
          .onSecondary = rgbHex(onSecondary->toArgb() & 0x00FFFFFFU),
          .tertiary = rgbHex(tertiary->toArgb() & 0x00FFFFFFU),
          .onTertiary = rgbHex(onTertiary->toArgb() & 0x00FFFFFFU),
          .error = rgbHex(error->toArgb() & 0x00FFFFFFU),
          .onError = rgbHex(onError->toArgb() & 0x00FFFFFFU),
          .surface = rgbHex(surface->toArgb() & 0x00FFFFFFU),
          .onSurface = rgbHex(onSurface->toArgb() & 0x00FFFFFFU),
          .surfaceVariant = rgbHex(surfaceVariant->toArgb() & 0x00FFFFFFU),
          .onSurfaceVariant = rgbHex(onSurfaceVariant->toArgb() & 0x00FFFFFFU),
          .outline = rgbHex(outlineRaw->toArgb() & 0x00FFFFFFU),
          .shadow = rgbHex(shadow.toArgb() & 0x00FFFFFFU),
          .hover = rgbHex(tertiary->toArgb() & 0x00FFFFFFU),
          .onHover = rgbHex(onTertiary->toArgb() & 0x00FFFFFFU),
      };
    }

    void injectTerminalColors(TokenMap& dst, const nlohmann::json& modeJson) {
      if (!modeJson.contains(kTerminalJsonKey) || !modeJson[kTerminalJsonKey].is_object())
        return;
      const auto& terminal = modeJson[kTerminalJsonKey];
      for (const auto& [jsonKey, flatKey] : kTerminalDirectColorTokenKeys) {
        if (terminal.contains(jsonKey) && terminal[jsonKey].is_string())
          setToken(dst, flatKey, terminal[jsonKey].get<std::string>());
      }
      for (const auto& group : kTerminalAnsiGroupTokenKeys) {
        if (!terminal.contains(group.jsonKey) || !terminal[group.jsonKey].is_object())
          continue;
        for (const auto key : kTerminalAnsiColorJsonKeys) {
          const auto& groupJson = terminal[group.jsonKey];
          if (!groupJson.contains(key) || !groupJson[key].is_string())
            continue;
          setToken(dst, std::string(group.tokenPrefix) + "_" + std::string(key), groupJson[key].get<std::string>());
        }
      }
    }

    bool loadThemeJson(const std::filesystem::path& path, GeneratedPalette& palette, std::string& err) {
      std::ifstream f(path);
      if (!f) {
        err = "cannot open theme json";
        return false;
      }

      nlohmann::json root;
      try {
        f >> root;
      } catch (const std::exception& e) {
        err = e.what();
        return false;
      }

      auto loadTokenMode = [](const nlohmann::json& src, TokenMap& dst) {
        if (!src.is_object())
          return;
        for (auto it = src.begin(); it != src.end(); ++it) {
          if (!it.value().is_string())
            continue;
          try {
            dst[it.key()] = Color::fromHex(it.value().get<std::string>()).toArgb();
          } catch (...) {
          }
        }
      };

      auto loadFixedPalette = [&](const nlohmann::json& src, std::string_view mode, TokenMap& dst) -> bool {
        auto parsed = parseFixedPaletteJson(src, err);
        if (!parsed)
          return false;
        dst = expandFixedPaletteMode(*parsed, mode == "dark");
        injectTerminalColors(dst, src);
        return true;
      };

      auto isFixedPaletteMode = [](const nlohmann::json& src) { return src.is_object() && src.contains("mPrimary"); };

      if (root.contains("dark") || root.contains("light")) {
        if (root.contains("dark")) {
          if (isFixedPaletteMode(root["dark"])) {
            if (!loadFixedPalette(root["dark"], "dark", palette.dark))
              return false;
          } else {
            loadTokenMode(root["dark"], palette.dark);
          }
        }
        if (root.contains("light")) {
          if (isFixedPaletteMode(root["light"])) {
            if (!loadFixedPalette(root["light"], "light", palette.light))
              return false;
          } else {
            loadTokenMode(root["light"], palette.light);
          }
        }
      } else if (isFixedPaletteMode(root)) {
        if (!loadFixedPalette(root, "dark", palette.dark) || !loadFixedPalette(root, "light", palette.light))
          return false;
      } else {
        loadTokenMode(root, palette.dark);
      }

      if (palette.dark.empty() && palette.light.empty()) {
        err = "theme json contained no token maps";
        return false;
      }
      synthesizeTerminalPaletteTokens(palette);
      return true;
    }

  } // namespace

  int runCli(int argc, char* argv[]) {
    const char* imagePath = nullptr;
    const char* themeJsonPath = nullptr;
    std::string schemeName = "m3-tonal-spot";
    Variant variant = Variant::Dark;
    const char* outPath = nullptr;
    const char* configPath = nullptr;
    std::string builtinConfigPathStorage;
    bool builtinConfig = false;
    bool listTemplatesRequested = false;
    std::string defaultMode = "dark";
    std::vector<std::string> renderSpecs;

    for (int i = 2; i < argc; ++i) {
      const char* a = argv[i];
      if (std::strcmp(a, "--help") == 0) {
        std::puts(kHelpText);
        return 0;
      }
      if (std::strcmp(a, "--scheme") == 0 && i + 1 < argc) {
        schemeName = argv[++i];
        continue;
      }
      if (std::strcmp(a, "--theme-json") == 0 && i + 1 < argc) {
        themeJsonPath = argv[++i];
        continue;
      }
      if (std::strcmp(a, "--dark") == 0) {
        variant = Variant::Dark;
        continue;
      }
      if (std::strcmp(a, "--light") == 0) {
        variant = Variant::Light;
        continue;
      }
      if (std::strcmp(a, "--both") == 0) {
        variant = Variant::Both;
        continue;
      }
      if (std::strcmp(a, "-o") == 0 && i + 1 < argc) {
        outPath = argv[++i];
        continue;
      }
      if ((std::strcmp(a, "--render") == 0 || std::strcmp(a, "-r") == 0) && i + 1 < argc) {
        renderSpecs.emplace_back(argv[++i]);
        continue;
      }
      if ((std::strcmp(a, "--config") == 0 || std::strcmp(a, "-c") == 0) && i + 1 < argc) {
        configPath = argv[++i];
        continue;
      }
      if (std::strcmp(a, "--builtin-config") == 0) {
        builtinConfig = true;
        continue;
      }
      if (std::strcmp(a, "--list-templates") == 0) {
        listTemplatesRequested = true;
        continue;
      }
      if (std::strcmp(a, "--default-mode") == 0 && i + 1 < argc) {
        defaultMode = argv[++i];
        continue;
      }
      if (!imagePath && a[0] != '-') {
        imagePath = a;
        continue;
      }
      std::fprintf(stderr, "error: unknown theme argument: %s\n", a);
      return 1;
    }

    if (listTemplatesRequested)
      return listTemplates(configPath);

    if (builtinConfig) {
      if (configPath != nullptr) {
        std::fputs("error: --builtin-config cannot be combined with --config\n", stderr);
        return 1;
      }
      builtinConfigPathStorage = builtinTemplateConfigPath().string();
      configPath = builtinConfigPathStorage.c_str();
    }

    if (!imagePath && !themeJsonPath) {
      std::fputs("error: theme requires an image path or --theme-json (try: noctalia theme --help)\n", stderr);
      return 1;
    }

    auto schemeOpt = schemeFromString(schemeName);
    if (!schemeOpt) {
      std::fprintf(stderr, "error: unknown scheme '%s'\n", schemeName.c_str());
      return 1;
    }

    std::string err;
    GeneratedPalette palette;
    if (themeJsonPath) {
      if (!loadThemeJson(FileUtils::expandUserPath(themeJsonPath), palette, err)) {
        std::fprintf(stderr, "error: failed to load theme json: %s\n", err.c_str());
        return 1;
      }
    } else {
      auto loaded = loadAndResize(imagePath, *schemeOpt, &err);
      if (!loaded) {
        std::fprintf(stderr, "error: failed to load image: %s\n", err.c_str());
        return 1;
      }

      palette = generate(loaded->rgb, *schemeOpt, &err);
      if (palette.dark.empty() && palette.light.empty()) {
        std::fprintf(stderr, "error: palette generation failed: %s\n", err.empty() ? "unknown error" : err.c_str());
        return 1;
      }
    }

    const std::string json = toJson(palette, *schemeOpt, variant);
    const bool hasTemplateWork = !renderSpecs.empty() || configPath != nullptr;
    if (outPath) {
      std::ofstream f(outPath);
      if (!f) {
        std::fprintf(stderr, "error: cannot open output file: %s\n", outPath);
        return 1;
      }
      f << json << '\n';
    } else if (!hasTemplateWork) {
      std::fwrite(json.data(), 1, json.size(), stdout);
      std::fputc('\n', stdout);
    }

    if (hasTemplateWork) {
      TemplateEngine::Options options;
      options.defaultMode = defaultMode;
      options.imagePath = imagePath ? imagePath : "";
      options.closestColor.clear();
      options.schemeType = schemeName.rfind("m3-", 0) == 0 ? schemeName.substr(3) : schemeName;
      options.verbose = true;
      TemplateEngine engine(TemplateEngine::makeThemeData(palette), std::move(options));

      for (const auto& spec : renderSpecs) {
        const size_t colon = spec.find(':');
        if (colon == std::string::npos) {
          std::fprintf(stderr, "error: invalid render spec (expected input:output): %s\n", spec.c_str());
          return 1;
        }
        const std::filesystem::path input = FileUtils::expandUserPath(spec.substr(0, colon));
        const std::filesystem::path output = FileUtils::expandUserPath(spec.substr(colon + 1));
        const auto result = engine.renderFile(input, output);
        if (!result.success)
          return 1;
      }

      if (configPath && !engine.processConfigFile(configPath))
        return 1;
    }

    return 0;
  }

} // namespace noctalia::theme
