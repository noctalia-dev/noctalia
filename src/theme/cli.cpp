#include "theme/cli.h"

#include "cli/parse.h"
#include "cli/schema_theme.h"
#include "config/config_export.h"
#include "config/config_service.h"
#include "core/files/resource_paths.h"
#include "core/toml.h" // IWYU pragma: keep
#include "theme/builtin_templates.h"
#include "theme/color.h"
#include "theme/community_templates.h"
#include "theme/fixed_palette.h"
#include "theme/image_loader.h"
#include "theme/json_output.h"
#include "theme/palette_generator.h"
#include "theme/palette_transform.h"
#include "theme/scheme.h"
#include "theme/template_engine.h"
#include "util/file_utils.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <print>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace noctalia::theme {

  namespace {
    std::filesystem::path builtinTemplateConfigPath() { return paths::assetPath("templates/builtin.toml"); }

    using TokenMap = std::unordered_map<std::string, uint32_t>;

    struct TemplateListEntry {
      std::string id;
      std::string category;
      std::string name;
    };

    std::string templateNameOrId(const std::string& id, const std::string& name) { return name.empty() ? id : name; }

    void sortTemplateList(std::vector<TemplateListEntry>& entries) {
      std::ranges::sort(entries, [](const TemplateListEntry& lhs, const TemplateListEntry& rhs) {
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
        std::println();
      firstGroup = false;
      std::println("{}", title);

      std::size_t idWidth = std::strlen("ID");
      std::size_t categoryWidth = std::strlen("Category");
      for (const auto& entry : entries) {
        idWidth = std::max(idWidth, entry.id.size());
        categoryWidth = std::max(categoryWidth, entry.category.empty() ? std::size_t{1} : entry.category.size());
      }

      const auto idColumn = static_cast<int>(idWidth);
      const auto categoryColumn = static_cast<int>(categoryWidth);
      std::println("  {:{}}  {:{}}  {}", "ID", idColumn, "Category", categoryColumn, "Name");
      for (const auto& entry : entries) {
        const std::string category = entry.category.empty() ? "-" : entry.category;
        std::println("  {:{}}  {:{}}  {}", entry.id, idColumn, category, categoryColumn, entry.name);
      }
    }

    int listTemplates(const char* configPath) {
      std::string err;
      const auto builtins = loadBuiltinTemplateList(err);
      if (!err.empty()) {
        std::println(stderr, "error: failed to load built-in templates: {}", err);
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
          std::println(stderr, "error: failed to load template config {}: {}", templateConfigPath.string(), userErr);
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
        std::println("No templates found.");
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

      if (!primary
          || !onPrimary
          || !secondary
          || !onSecondary
          || !tertiary
          || !onTertiary
          || !error
          || !onError
          || !surface
          || !onSurface
          || !surfaceVariant
          || !onSurfaceVariant
          || !outlineRaw) {
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
    const auto parsed = cli_schema::parseArgs(argc, argv, 2, kThemeCmd);
    if (!parsed) {
      std::println(stderr, "error: {}", parsed.error());
      std::println(stderr, "Run 'noctalia theme --help' for usage.");
      return 1;
    }
    if (parsed->helpRequested) {
      return 0;
    }

    const char* imagePath = parsed->positionals.empty() ? nullptr : parsed->positionals[0].c_str();
    std::string schemeName = parsed->hasFlag("--scheme") ? std::string(parsed->flagValue("--scheme")) : "m3-tonal-spot";

    Variant variant = Variant::Dark;
    if (parsed->hasFlag("--light"))
      variant = Variant::Light;
    if (parsed->hasFlag("--both"))
      variant = Variant::Both;

    bool pureBlack = parsed->hasFlag("--pure-black");
    const char* themeJsonPath = parsed->hasFlag("--theme-json") ? parsed->flagValue("--theme-json").data() : nullptr;
    const char* outPath = parsed->hasFlag("-o") ? parsed->flagValue("-o").data() : nullptr;

    std::vector<std::string> renderSpecs;

    const char* configPath = parsed->hasFlag("-c")
        ? parsed->flagValue("-c").data()
        : (parsed->hasFlag("--config") ? parsed->flagValue("--config").data() : nullptr);
    bool builtinConfig = parsed->hasFlag("--builtin-config");
    bool listTemplatesRequested = parsed->hasFlag("--list-templates");
    std::string defaultMode =
        parsed->hasFlag("--default-mode") ? std::string(parsed->flagValue("--default-mode")) : "dark";

    for (int i = 2; i < argc; ++i) {
      std::string_view a = argv[i];
      if ((a == "--render" || a == "-r") && i + 1 < argc) {
        renderSpecs.emplace_back(argv[++i]);
      }
    }

    std::string builtinConfigPathStorage;
    if (listTemplatesRequested)
      return listTemplates(configPath);

    if (builtinConfig) {
      if (configPath != nullptr) {
        std::println(stderr, "error: --builtin-config cannot be combined with --config");
        return 1;
      }
      builtinConfigPathStorage = builtinTemplateConfigPath().string();
      configPath = builtinConfigPathStorage.c_str();
    }

    if (!imagePath && !themeJsonPath) {
      std::println(stderr, "error: theme requires an image path or --theme-json (try: noctalia theme --help)");
      return 1;
    }

    auto schemeOpt = schemeFromString(schemeName);
    if (!schemeOpt) {
      std::println(stderr, "error: unknown scheme '{}'", schemeName);
      return 1;
    }

    std::string err;
    GeneratedPalette palette;
    if (themeJsonPath) {
      if (!loadThemeJson(FileUtils::expandUserPath(themeJsonPath), palette, err)) {
        std::println(stderr, "error: failed to load theme json: {}", err);
        return 1;
      }
    } else {
      auto loaded = loadAndResize(imagePath, *schemeOpt);
      if (!loaded) {
        std::println(stderr, "error: failed to load image: {}", loaded.error());
        return 1;
      }

      auto generated = generate(loaded->rgb, *schemeOpt);
      if (!generated) {
        std::println(stderr, "error: palette generation failed: {}", generated.error());
        return 1;
      }
      palette = std::move(*generated);
    }

    if (pureBlack) {
      applyPureBlackDark(palette);
    }

    const std::string json = toJson(palette, *schemeOpt, variant);
    const bool hasTemplateWork = !renderSpecs.empty() || configPath != nullptr;
    if (outPath) {
      std::ofstream f(outPath);
      if (!f) {
        std::println(stderr, "error: cannot open output file: {}", outPath);
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
      options.schemeType = schemeName.starts_with("m3-") ? schemeName.substr(3) : schemeName;
      options.verbose = true;
      ConfigService config;
      options.configTable = std::make_shared<toml::table>(config_export::serialize(config.config()));
      TemplateEngine engine(TemplateEngine::makeThemeData(palette), std::move(options));

      // Custom colors from -c must be in the theme data before -r templates render.
      toml::table configRoot;
      std::filesystem::path templateConfigPath;
      if (configPath) {
        templateConfigPath = FileUtils::expandUserPath(configPath);
        try {
          configRoot = toml::parse_file(templateConfigPath.string());
        } catch (const toml::parse_error& e) {
          std::println(
              stderr, "error: failed to parse template config {}: {}", templateConfigPath.string(), e.description()
          );
          return 1;
        }
        engine.applyCustomColors(configRoot);
      }

      for (const auto& spec : renderSpecs) {
        const size_t colon = spec.find(':');
        if (colon == std::string::npos) {
          std::println(stderr, "error: invalid render spec (expected input:output): {}", spec);
          return 1;
        }
        const std::filesystem::path input = FileUtils::expandUserPath(spec.substr(0, colon));
        const std::filesystem::path output = FileUtils::expandUserPath(spec.substr(colon + 1));
        const auto result = engine.renderFile(input, output);
        if (!result.success)
          return 1;
      }

      if (configPath && !engine.processConfigTemplates(configRoot, templateConfigPath))
        return 1;
    }

    return 0;
  }

} // namespace noctalia::theme
