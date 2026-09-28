#include "scripting/plugin_api.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_panel_shell.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "plugin_manifest_test: {}", message);
    }
    return condition;
  }

  bool expectEq(std::string_view actual, std::string_view expected, const char* message) {
    if (actual != expected) {
      std::println(stderr, "plugin_manifest_test: {}\n  actual:   {}\n  expected: {}", message, actual, expected);
      return false;
    }
    return true;
  }

  std::filesystem::path makeTempDir() {
    std::string pattern = (std::filesystem::temp_directory_path() / "noctalia-plugin-manifest-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char* result = ::mkdtemp(buffer.data());
    return result != nullptr ? std::filesystem::path(result) : std::filesystem::path{};
  }

  bool writeText(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    out << text;
    return out.good();
  }

  bool writeManifest(const std::filesystem::path& path, std::string_view text) {
    std::string manifest{"version = \"1.0.0\"\n"};
    manifest.append(text);
    return writeText(path, manifest);
  }

} // namespace

int main() {
  const auto root = makeTempDir();
  if (!expect(!root.empty(), "failed to create temp dir")) {
    return 1;
  }

  bool ok = true;
  static_assert(scripting::kOldestSupportedPluginApiVersion > 0);
  ok = expect(
           !scripting::supportsPluginApiVersion(scripting::kOldestSupportedPluginApiVersion - 1),
           "plugin API before oldest should be too old"
       )
      && ok;
  ok = expect(
           scripting::supportsPluginApiVersion(scripting::kOldestSupportedPluginApiVersion),
           "oldest plugin API should be supported"
       )
      && ok;
  ok = expect(
           scripting::supportsPluginApiVersion(scripting::kCurrentPluginApiVersion),
           "current plugin API should be supported"
       )
      && ok;
  if constexpr (scripting::kCurrentPluginApiVersion < std::numeric_limits<std::uint32_t>::max()) {
    ok = expect(
             !scripting::supportsPluginApiVersion(scripting::kCurrentPluginApiVersion + 1),
             "plugin API after current should be too new"
         )
        && ok;
  }
  const auto defaultManifestPath = root / "defaults/plugin.toml";
  ok = writeManifest(defaultManifestPath, "id = \"me/defaults\"\nname = \"Defaults\"\nplugin_api = 3\n") && ok;

  std::string error;
  const auto defaults = scripting::parsePluginManifest(defaultManifestPath, &error);
  ok = expect(defaults.has_value(), error.empty() ? "failed to parse default manifest" : error.c_str()) && ok;
  if (defaults.has_value()) {
    ok = expect(defaults->pluginApiVersion == 3, "plugin API version should parse") && ok;
    ok = expectEq(defaults->license, "MIT", "license should default to MIT") && ok;
    ok = expect(!defaults->deprecated, "deprecated should default to false") && ok;
    ok = expect(defaults->dependencies.empty(), "dependencies should default to empty") && ok;
  }
  ok = expect(scripting::isValidPluginVersion("0.0.0"), "zero version should be valid") && ok;
  ok = expect(scripting::isValidPluginVersion("10.20.30"), "multi-digit version should be valid") && ok;
  ok = expect(!scripting::isValidPluginVersion("1.2"), "two-component version should fail") && ok;
  ok = expect(!scripting::isValidPluginVersion("1.2.3.4"), "four-component version should fail") && ok;
  ok = expect(!scripting::isValidPluginVersion("01.2.3"), "leading-zero major version should fail") && ok;
  ok = expect(!scripting::isValidPluginVersion("1.02.3"), "leading-zero minor version should fail") && ok;
  ok = expect(!scripting::isValidPluginVersion("1.2.03"), "leading-zero patch version should fail") && ok;
  ok = expect(!scripting::isValidPluginVersion("1.2.3-beta.1"), "prerelease version should fail") && ok;
  ok = expect(!scripting::isValidPluginVersion("1.2.three"), "non-numeric version should fail") && ok;

  const auto missingVersionPath = root / "missing-version/plugin.toml";
  ok = writeText(missingVersionPath, "id = \"me/missing-version\"\nname = \"Missing Version\"\nplugin_api = 3\n") && ok;
  error.clear();
  const auto missingVersion = scripting::parsePluginManifest(missingVersionPath, &error);
  ok = expect(!missingVersion.has_value(), "manifest without version should fail") && ok;
  ok = expectEq(error, "missing mandatory key 'version'", "missing version error") && ok;

  const auto invalidVersionPath = root / "invalid-version/plugin.toml";
  ok = writeText(
           invalidVersionPath,
           "id = \"me/invalid-version\"\n"
           "name = \"Invalid Version\"\n"
           "version = \"1.2.3-beta.1\"\n"
           "plugin_api = 3\n"
       )
      && ok;
  error.clear();
  const auto invalidVersion = scripting::parsePluginManifest(invalidVersionPath, &error);
  ok = expect(!invalidVersion.has_value(), "manifest with prerelease version should fail") && ok;
  ok = expectEq(error, "invalid 'version' (expected MAJOR.MINOR.PATCH)", "invalid version error") && ok;

  const auto explicitManifestPath = root / "explicit/plugin.toml";
  ok = writeManifest(
           explicitManifestPath,
           "id = \"me/explicit\"\n"
           "name = \"Explicit\"\n"
           "plugin_api = 3\n"
           "license = \"Apache-2.0\"\n"
           "deprecated = true\n"
           "dependencies = [\"grim\", \"slurp\"]\n"
       )
      && ok;

  error.clear();
  const auto explicitManifest = scripting::parsePluginManifest(explicitManifestPath, &error);
  ok = expect(explicitManifest.has_value(), error.empty() ? "failed to parse explicit manifest" : error.c_str()) && ok;
  if (explicitManifest.has_value()) {
    ok = expectEq(explicitManifest->license, "Apache-2.0", "license should parse explicit value") && ok;
    ok = expect(explicitManifest->deprecated, "deprecated should parse explicit value") && ok;
    ok = expect(explicitManifest->dependencies.size() == 2, "dependencies should parse explicit values") && ok;
    if (explicitManifest->dependencies.size() == 2) {
      ok = expectEq(explicitManifest->dependencies[0], "grim", "first dependency") && ok;
      ok = expectEq(explicitManifest->dependencies[1], "slurp", "second dependency") && ok;
    }
  }

  const auto translatedSettingsManifestPath = root / "translated-settings/plugin.toml";
  ok = writeManifest(
           translatedSettingsManifestPath,
           "id = \"me/translated-settings\"\n"
           "name = \"Translated Settings\"\n"
           "plugin_api = 3\n"
           "[[setting]]\n"
           "key = \"mode\"\n"
           "type = \"select\"\n"
           "label_key = \"settings.mode.label\"\n"
           "description_key = \"settings.mode.description\"\n"
           "default = \"auto\"\n"
           "options = [\n"
           "  { value = \"auto\", label_key = \"settings.mode.options.auto\" },\n"
           "  { value = \"manual\", label_key = \"settings.mode.options.manual\" },\n"
           "]\n"
           "[[widget]]\n"
           "id = \"hello\"\n"
           "entry = \"hello.luau\"\n"
           "[[widget.setting]]\n"
           "key = \"label\"\n"
           "type = \"string\"\n"
           "label_key = \"settings.label.label\"\n"
       )
      && ok;
  error.clear();
  const auto translatedSettingsManifest = scripting::parsePluginManifest(translatedSettingsManifestPath, &error);
  ok = expect(
           translatedSettingsManifest.has_value(),
           error.empty() ? "failed to parse translated settings manifest" : error.c_str()
       )
      && ok;
  if (translatedSettingsManifest.has_value()) {
    ok = expect(translatedSettingsManifest->settings.size() == 1, "one plugin setting expected") && ok;
    if (!translatedSettingsManifest->settings.empty()) {
      const auto& setting = translatedSettingsManifest->settings.front();
      ok = expectEq(setting.labelKey, "settings.mode.label", "setting label_key should parse") && ok;
      ok = expectEq(setting.descriptionKey, "settings.mode.description", "setting description_key should parse") && ok;
      ok = expect(setting.options.size() == 2, "two select options expected") && ok;
      if (setting.options.size() == 2) {
        ok = expectEq(setting.options[0].labelKey, "settings.mode.options.auto", "select option label_key should parse")
            && ok;
        ok = expectEq(
                 setting.options[1].labelKey, "settings.mode.options.manual", "second option label_key should parse"
             )
            && ok;
      }
    }
    ok = expect(translatedSettingsManifest->entries.size() == 1, "one translated widget entry expected") && ok;
    if (!translatedSettingsManifest->entries.empty()) {
      const auto& settings = translatedSettingsManifest->entries.front().settings;
      ok = expect(settings.size() == 1, "one translated widget setting expected") && ok;
      if (!settings.empty()) {
        ok = expectEq(settings.front().labelKey, "settings.label.label", "widget setting label_key should parse") && ok;
      }
    }
  }

  const auto pathSettingsManifestPath = root / "path-settings/plugin.toml";
  ok = writeManifest(
           pathSettingsManifestPath,
           "id = \"me/path-settings\"\n"
           "name = \"Path Settings\"\n"
           "plugin_api = 3\n"
           "[[setting]]\n"
           "key = \"config_file\"\n"
           "type = \"file\"\n"
           "label_key = \"settings.config_file.label\"\n"
           "default = \"\"\n"
           "extensions = [\".toml\", \".json\"]\n"
           "[[desktop_widget]]\n"
           "id = \"notes\"\n"
           "entry = \"notes.luau\"\n"
           "[[desktop_widget.setting]]\n"
           "key = \"notes_dir\"\n"
           "type = \"folder\"\n"
           "label_key = \"settings.notes_dir.label\"\n"
           "default = \"~/Documents/Notes\"\n"
       )
      && ok;
  error.clear();
  const auto pathSettingsManifest = scripting::parsePluginManifest(pathSettingsManifestPath, &error);
  ok =
      expect(pathSettingsManifest.has_value(), error.empty() ? "failed to parse path settings manifest" : error.c_str())
      && ok;
  if (pathSettingsManifest.has_value()) {
    ok = expect(pathSettingsManifest->settings.size() == 1, "one plugin path setting expected") && ok;
    if (!pathSettingsManifest->settings.empty()) {
      const auto& fileSetting = pathSettingsManifest->settings.front();
      ok = expect(fileSetting.type == scripting::ManifestFieldType::File, "file setting type should parse") && ok;
      ok = expect(fileSetting.extensions.size() == 2, "file setting extensions should parse") && ok;
    }
    ok = expect(pathSettingsManifest->entries.size() == 1, "one desktop widget entry expected") && ok;
    if (!pathSettingsManifest->entries.empty()) {
      ok =
          expect(pathSettingsManifest->entries.front().settings.size() == 1, "one desktop widget path setting expected")
          && ok;
    }
    if (!pathSettingsManifest->entries.empty() && !pathSettingsManifest->entries.front().settings.empty()) {
      ok = expect(
               pathSettingsManifest->entries.front().settings.front().type == scripting::ManifestFieldType::Folder,
               "desktop widget folder setting type should parse"
           )
          && ok;
    }
  }

  const auto literalLabelPath = root / "literal-label/plugin.toml";
  ok = writeManifest(
           literalLabelPath,
           "id = \"me/literal-label\"\n"
           "name = \"Literal Label\"\n"
           "plugin_api = 3\n"
           "[[setting]]\n"
           "key = \"mode\"\n"
           "label = \"Mode\"\n"
       )
      && ok;
  error.clear();
  const auto literalLabel = scripting::parsePluginManifest(literalLabelPath, &error);
  ok = expect(!literalLabel.has_value(), "literal label should fail") && ok;
  ok = expectEq(
           error, "setting 'mode' uses 'label'; use 'label_key' that points to translation key instead",
           "literal label error"
       )
      && ok;

  const auto literalDescriptionPath = root / "literal-description/plugin.toml";
  ok = writeManifest(
           literalDescriptionPath,
           "id = \"me/literal-description\"\n"
           "name = \"Literal Description\"\n"
           "plugin_api = 3\n"
           "[[setting]]\n"
           "key = \"mode\"\n"
           "label_key = \"settings.mode.label\"\n"
           "description = \"Mode\"\n"
       )
      && ok;
  error.clear();
  const auto literalDescription = scripting::parsePluginManifest(literalDescriptionPath, &error);
  ok = expect(!literalDescription.has_value(), "literal description should fail") && ok;
  ok = expectEq(
           error, "setting 'mode' uses 'description'; use 'description_key' that points to translation key instead",
           "literal description error"
       )
      && ok;

  const auto missingLabelKeyPath = root / "missing-label-key/plugin.toml";
  ok = writeManifest(
           missingLabelKeyPath,
           "id = \"me/missing-label-key\"\n"
           "name = \"Missing Label Key\"\n"
           "plugin_api = 3\n"
           "[[setting]]\n"
           "key = \"mode\"\n"
       )
      && ok;
  error.clear();
  const auto missingLabelKey = scripting::parsePluginManifest(missingLabelKeyPath, &error);
  ok = expect(!missingLabelKey.has_value(), "setting without label_key should fail") && ok;
  ok = expectEq(error, "setting 'mode' is missing 'label_key'", "missing label_key error") && ok;

  const auto literalOptionLabelPath = root / "literal-option-label/plugin.toml";
  ok = writeManifest(
           literalOptionLabelPath,
           "id = \"me/literal-option-label\"\n"
           "name = \"Literal Option Label\"\n"
           "plugin_api = 3\n"
           "[[setting]]\n"
           "key = \"mode\"\n"
           "type = \"select\"\n"
           "label_key = \"settings.mode.label\"\n"
           "default = \"auto\"\n"
           "options = [{ value = \"auto\", label = \"Auto\" }]\n"
       )
      && ok;
  error.clear();
  const auto literalOptionLabel = scripting::parsePluginManifest(literalOptionLabelPath, &error);
  ok = expect(!literalOptionLabel.has_value(), "literal select option label should fail") && ok;
  ok = expectEq(
           error, "setting 'mode' option 'auto' uses 'label'; use 'label_key' that points to translation key instead",
           "literal option label error"
       )
      && ok;

  const auto missingOptionLabelKeyPath = root / "missing-option-label-key/plugin.toml";
  ok = writeManifest(
           missingOptionLabelKeyPath,
           "id = \"me/missing-option-label-key\"\n"
           "name = \"Missing Option Label Key\"\n"
           "plugin_api = 3\n"
           "[[setting]]\n"
           "key = \"mode\"\n"
           "type = \"select\"\n"
           "label_key = \"settings.mode.label\"\n"
           "default = \"auto\"\n"
           "options = [\"auto\", \"manual\"]\n"
       )
      && ok;
  error.clear();
  const auto missingOptionLabelKey = scripting::parsePluginManifest(missingOptionLabelKeyPath, &error);
  ok = expect(!missingOptionLabelKey.has_value(), "bare string select options should fail") && ok;
  ok = expectEq(error, "setting 'mode' option must be a table with value and label_key", "bare option error") && ok;

  const auto launcherManifestPath = root / "launcher/plugin.toml";
  ok = writeManifest(
           launcherManifestPath,
           "id = \"me/launcher\"\n"
           "name = \"Launcher\"\n"
           "plugin_api = 3\n"
           "[[launcher_provider]]\n"
           "id = \"translate\"\n"
           "entry = \"translate.luau\"\n"
           "prefix = \":tr\"\n"
           "glyph = \"language\"\n"
           "include_in_global_search = true\n"
           "debounce_ms = 300\n"
           "[[launcher_provider.category]]\n"
           "label = \"Languages\"\n"
           "glyph = \"world\"\n"
       )
      && ok;
  error.clear();
  const auto launcherManifest = scripting::parsePluginManifest(launcherManifestPath, &error);
  ok = expect(launcherManifest.has_value(), error.empty() ? "failed to parse launcher manifest" : error.c_str()) && ok;
  if (launcherManifest.has_value() && expect(launcherManifest->entries.size() == 1, "one launcher entry expected")) {
    const auto& entry = launcherManifest->entries.front();
    ok = expect(entry.kind == scripting::PluginEntryKind::LauncherProvider, "entry kind should be LauncherProvider")
        && ok;
    ok = expectEq(entry.launcherPrefix, ":tr", "launcher prefix should parse") && ok;
    ok = expectEq(entry.launcherGlyph, "language", "launcher glyph should parse") && ok;
    ok = expect(entry.launcherGlobalSearch, "include_in_global_search should parse true") && ok;
    ok = expect(entry.launcherDebounceMs == 300, "debounce_ms should parse") && ok;
    ok = expect(entry.launcherCategories.size() == 1, "one launcher category expected") && ok;
    if (!entry.launcherCategories.empty()) {
      ok = expectEq(entry.launcherCategories.front().label, "Languages", "category label should parse") && ok;
      ok = expectEq(entry.launcherCategories.front().glyph, "world", "category glyph should parse") && ok;
    }
  }

  // Entry-level settings on a launcher provider (a singleton with no settings UI)
  // are rejected — authors must use a plugin-level [[setting]] instead.
  const auto launcherSettingManifestPath = root / "launcher-setting/plugin.toml";
  ok = writeManifest(
           launcherSettingManifestPath,
           "id = \"me/launcher-setting\"\n"
           "name = \"Launcher Setting\"\n"
           "plugin_api = 3\n"
           "[[launcher_provider]]\n"
           "id = \"translate\"\n"
           "entry = \"translate.luau\"\n"
           "[[launcher_provider.setting]]\n"
           "key = \"target_lang\"\n"
           "type = \"string\"\n"
           "default = \"en\"\n"
       )
      && ok;
  error.clear();
  const auto launcherSettingManifest = scripting::parsePluginManifest(launcherSettingManifestPath, &error);
  ok = expect(!launcherSettingManifest.has_value(), "launcher-provider entry setting should be rejected") && ok;
  ok = expectEq(
           error,
           "entry 'translate' of kind 'launcher_provider' declares [[launcher_provider.setting]], but entry-level "
           "settings are only supported for widget, desktop_widget, and panel entries; move it to a plugin-level "
           "[[setting]]",
           "launcher-provider entry setting error message"
       )
      && ok;

  const auto listManifestPath = root / "string-list/plugin.toml";
  ok = writeManifest(
           listManifestPath,
           "id = \"me/string-list\"\n"
           "name = \"String List\"\n"
           "plugin_api = 3\n"
           "[[widget]]\n"
           "id = \"list\"\n"
           "entry = \"list.luau\"\n"
           "[[widget.setting]]\n"
           "key = \"paths\"\n"
           "type = \"string_list\"\n"
           "label_key = \"settings.paths.label\"\n"
           "default = [\"/dev/input/by-id/a\", \"/dev/input/by-path/b\"]\n"
       )
      && ok;
  error.clear();
  const auto listManifest = scripting::parsePluginManifest(listManifestPath, &error);
  ok = expect(listManifest.has_value(), error.empty() ? "failed to parse string-list manifest" : error.c_str()) && ok;
  if (listManifest.has_value() && expect(listManifest->entries.size() == 1, "one string-list entry expected")) {
    const auto& settings = listManifest->entries.front().settings;
    ok = expect(settings.size() == 1, "one string-list setting expected") && ok;
    if (!settings.empty()) {
      ok = expect(settings.front().type == scripting::ManifestFieldType::StringList, "setting should be StringList")
          && ok;
      const auto defaultValue = settings.front().defaultValue();
      const auto* values = std::get_if<std::vector<std::string>>(&defaultValue);
      ok = expect(values != nullptr, "string-list default should be a vector") && ok;
      if (values != nullptr) {
        ok = expect(values->size() == 2, "string-list default size") && ok;
        if (values->size() == 2) {
          ok = expectEq((*values)[0], "/dev/input/by-id/a", "first string-list default") && ok;
          ok = expectEq((*values)[1], "/dev/input/by-path/b", "second string-list default") && ok;
        }
      }
    }
  }

  const auto mapManifestPath = root / "string-map/plugin.toml";
  ok = writeManifest(
           mapManifestPath,
           "id = \"me/string-map\"\n"
           "name = \"String Map\"\n"
           "plugin_api = 6\n"
           "[[widget]]\n"
           "id = \"outputs\"\n"
           "entry = \"outputs.luau\"\n"
           "[[widget.setting]]\n"
           "key = \"output_glyphs\"\n"
           "type = \"string_map\"\n"
           "label_key = \"settings.output_glyphs.label\"\n"
           "default = { \"eDP-1\" = \"laptop\", \"DP-1\" = \"monitor\" }\n"
       )
      && ok;
  error.clear();
  const auto mapManifest = scripting::parsePluginManifest(mapManifestPath, &error);
  ok = expect(mapManifest.has_value(), error.empty() ? "failed to parse string-map manifest" : error.c_str()) && ok;
  if (mapManifest.has_value() && expect(mapManifest->entries.size() == 1, "one string-map entry expected")) {
    const auto& settings = mapManifest->entries.front().settings;
    ok = expect(settings.size() == 1, "one string-map setting expected") && ok;
    if (!settings.empty()) {
      ok =
          expect(settings.front().type == scripting::ManifestFieldType::StringMap, "setting should be StringMap") && ok;
      const auto defaultValue = settings.front().defaultValue();
      const auto* values = std::get_if<WidgetSettingStringMap>(&defaultValue);
      ok = expect(values != nullptr, "string-map default should be a map") && ok;
      if (values != nullptr) {
        ok = expect(values->size() == 2, "string-map default size") && ok;
        ok = expect(values->at("eDP-1") == "laptop", "first string-map default") && ok;
        ok = expect(values->at("DP-1") == "monitor", "second string-map default") && ok;
      }
    }
  }

  const auto invalidMapManifestPath = root / "invalid-string-map/plugin.toml";
  ok = writeManifest(
           invalidMapManifestPath,
           "id = \"me/invalid-string-map\"\n"
           "name = \"Invalid String Map\"\n"
           "plugin_api = 6\n"
           "[[setting]]\n"
           "key = \"output_glyphs\"\n"
           "type = \"string_map\"\n"
           "label_key = \"settings.output_glyphs.label\"\n"
           "default = { \"eDP-1\" = 1 }\n"
       )
      && ok;
  error.clear();
  const auto invalidMapManifest = scripting::parsePluginManifest(invalidMapManifestPath, &error);
  ok = expect(!invalidMapManifest.has_value(), "string-map default with a non-string value should fail") && ok;
  ok = expectEq(error, "setting 'output_glyphs' string_map default values must be strings", "invalid string-map error")
      && ok;

  const auto oldApiMapManifestPath = root / "old-api-string-map/plugin.toml";
  ok = writeManifest(
           oldApiMapManifestPath,
           "id = \"me/old-api-string-map\"\n"
           "name = \"Old API String Map\"\n"
           "plugin_api = 5\n"
           "[[setting]]\n"
           "key = \"output_glyphs\"\n"
           "type = \"string_map\"\n"
           "label_key = \"settings.output_glyphs.label\"\n"
           "default = {}\n"
       )
      && ok;
  error.clear();
  const auto oldApiMapManifest = scripting::parsePluginManifest(oldApiMapManifestPath, &error);
  ok = expect(!oldApiMapManifest.has_value(), "string-map setting should require plugin API 6") && ok;
  ok =
      expectEq(error, "setting 'output_glyphs' type 'string_map' requires plugin_api >= 6", "string-map API gate error")
      && ok;

  const auto doubleManifestPath = root / "double-setting/plugin.toml";
  ok = writeManifest(
           doubleManifestPath,
           "id = \"me/double-setting\"\n"
           "name = \"Double Setting\"\n"
           "plugin_api = 6\n"
           "[[desktop_widget]]\n"
           "id = \"meter\"\n"
           "entry = \"meter.luau\"\n"
           "[[desktop_widget.setting]]\n"
           "key = \"opacity\"\n"
           "type = \"double\"\n"
           "label_key = \"settings.opacity.label\"\n"
           "default = 0.5\n"
           "min = 0.0\n"
           "max = 1.0\n"
           "step = 0.05\n"
       )
      && ok;
  error.clear();
  const auto doubleManifest = scripting::parsePluginManifest(doubleManifestPath, &error);
  ok = expect(doubleManifest.has_value(), error.empty() ? "failed to parse double manifest" : error.c_str()) && ok;
  if (doubleManifest.has_value() && expect(doubleManifest->entries.size() == 1, "one double entry expected")) {
    const auto& settings = doubleManifest->entries.front().settings;
    ok = expect(settings.size() == 1, "one double setting expected") && ok;
    if (!settings.empty()) {
      const auto& setting = settings.front();
      ok = expect(setting.type == scripting::ManifestFieldType::Double, "setting should be Double") && ok;
      ok = expect(setting.numberDefault == 0.5, "double default should parse") && ok;
      ok = expect(setting.minValue == 0.0, "double min should parse") && ok;
      ok = expect(setting.maxValue == 1.0, "double max should parse") && ok;
      ok = expect(std::abs(setting.step - 0.05) <= std::numeric_limits<double>::epsilon(), "double step should parse")
          && ok;
    }
  }

  const auto expectInvalidNumericSetting = [&](std::string_view fixtureName, std::string_view settingBody,
                                               std::string_view expectedError) {
    const auto manifestPath = root / std::filesystem::path(fixtureName) / "plugin.toml";
    const std::string manifest = "id = \"me/"
        + std::string(fixtureName)
        + "\"\n"
          "name = \"Invalid Numeric Setting\"\n"
          "plugin_api = 6\n"
          "[[setting]]\n"
          "key = \"value\"\n"
          "label_key = \"settings.value.label\"\n"
        + std::string(settingBody);
    bool result = writeManifest(manifestPath, manifest);
    error.clear();
    const auto parsed = scripting::parsePluginManifest(manifestPath, &error);
    result = expect(!parsed.has_value(), "invalid numeric setting should fail") && result;
    result = expectEq(error, expectedError, "invalid numeric setting error") && result;
    return result;
  };

  ok = expectInvalidNumericSetting(
           "double-string-default", "type = \"double\"\ndefault = \"fast\"\n",
           "setting 'value' double default must be a finite number"
       )
      && ok;
  ok = expectInvalidNumericSetting(
           "double-zero-step", "type = \"double\"\ndefault = 0.5\nstep = 0.0\n",
           "setting 'value' step must be greater than zero"
       )
      && ok;
  ok = expectInvalidNumericSetting(
           "double-inverted-range", "type = \"double\"\ndefault = 0.5\nmin = 1.0\nmax = 0.0\n",
           "setting 'value' min must be less than or equal to max"
       )
      && ok;
  ok = expectInvalidNumericSetting(
           "int-float-min", "type = \"int\"\ndefault = 2\nmin = 0.5\n", "setting 'value' min must be an integer"
       )
      && ok;
  ok = expectInvalidNumericSetting(
           "string-numeric-bound", "type = \"string\"\ndefault = \"value\"\nmin = 0\n",
           "setting 'value' min is only valid for int or double"
       )
      && ok;

  // Panel width/height: number, "fill", or a loud error — never a silent default.
  const auto fillPanelManifestPath = root / "fill-panel/plugin.toml";
  ok = writeManifest(
           fillPanelManifestPath,
           "id = \"me/fill-panel\"\n"
           "name = \"Fill Panel\"\n"
           "plugin_api = 3\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "width = 420\n"
           "height = \"fill\"\n"
           "placement = \"floating\"\n"
           "position = \"center_right\"\n"
       )
      && ok;
  error.clear();
  const auto fillPanel = scripting::parsePluginManifest(fillPanelManifestPath, &error);
  ok = expect(fillPanel.has_value(), error.empty() ? "failed to parse fill panel manifest" : error.c_str()) && ok;
  if (fillPanel.has_value() && expect(fillPanel->entries.size() == 1, "one fill panel entry expected")) {
    const auto& entry = fillPanel->entries.front();
    ok = expect(entry.panelWidth == 420.0, "fill panel width should parse") && ok;
    ok = expect(!entry.panelWidthFill, "numeric width is not fill") && ok;
    ok = expect(entry.panelHeightFill, "height \"fill\" should set the fill flag") && ok;
  }

  const auto badFillManifestPath = root / "bad-fill/plugin.toml";
  ok = writeManifest(
           badFillManifestPath,
           "id = \"me/bad-fill\"\n"
           "name = \"Bad Fill\"\n"
           "plugin_api = 3\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "height = \"full\"\n"
       )
      && ok;
  error.clear();
  const auto badFill = scripting::parsePluginManifest(badFillManifestPath, &error);
  ok = expect(!badFill.has_value(), "height \"full\" should fail loudly") && ok;
  ok = expectEq(error, "panel entry 'panel': height must be a positive number or \"fill\"", "bad fill error") && ok;

  const auto negativeSizeManifestPath = root / "negative-size/plugin.toml";
  ok = writeManifest(
           negativeSizeManifestPath,
           "id = \"me/negative-size\"\n"
           "name = \"Negative Size\"\n"
           "plugin_api = 3\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "width = -5\n"
       )
      && ok;
  error.clear();
  const auto negativeSize = scripting::parsePluginManifest(negativeSizeManifestPath, &error);
  ok = expect(!negativeSize.has_value(), "negative width should fail loudly") && ok;
  ok =
      expectEq(error, "panel entry 'panel': width must be a positive number or \"fill\"", "negative width error") && ok;

  const auto fillAttachedManifestPath = root / "fill-attached/plugin.toml";
  ok = writeManifest(
           fillAttachedManifestPath,
           "id = \"me/fill-attached\"\n"
           "name = \"Fill Attached\"\n"
           "plugin_api = 3\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "height = \"fill\"\n"
           "placement = \"attached\"\n"
       )
      && ok;
  error.clear();
  const auto fillAttached = scripting::parsePluginManifest(fillAttachedManifestPath, &error);
  ok = expect(!fillAttached.has_value(), "fill + attached placement should fail loudly") && ok;
  ok = expectEq(
           error, R"(panel entry 'panel': width/height "fill" requires placement = "floating")", "fill attached error"
       )
      && ok;

  const auto missingNameManifestPath = root / "missing-name/plugin.toml";
  ok = writeManifest(missingNameManifestPath, "id = \"me/missing-name\"\nplugin_api = 3\n") && ok;
  error.clear();
  const auto missingName = scripting::parsePluginManifest(missingNameManifestPath, &error);
  ok = expect(!missingName.has_value(), "manifest without name should fail") && ok;
  ok = expectEq(error, "missing mandatory key 'name'", "missing name error") && ok;

  const auto missingPluginApiPath = root / "missing-plugin-api/plugin.toml";
  ok = writeManifest(missingPluginApiPath, "id = \"me/missing-api\"\nname = \"Missing API\"\n") && ok;
  error.clear();
  const auto missingPluginApi = scripting::parsePluginManifest(missingPluginApiPath, &error);
  ok = expect(!missingPluginApi.has_value(), "manifest without plugin_api should fail") && ok;
  ok = expectEq(error, "missing mandatory key 'plugin_api'", "missing plugin API error") && ok;

  const auto invalidPluginApiPath = root / "invalid-plugin-api/plugin.toml";
  ok = writeManifest(invalidPluginApiPath, "id = \"me/invalid-api\"\nname = \"Invalid API\"\nplugin_api = \"3\"\n")
      && ok;
  error.clear();
  const auto invalidPluginApi = scripting::parsePluginManifest(invalidPluginApiPath, &error);
  ok = expect(!invalidPluginApi.has_value(), "string plugin_api should fail") && ok;
  ok = expectEq(error, "invalid 'plugin_api' (expected a positive integer)", "invalid plugin API error") && ok;

  const auto zeroPluginApiPath = root / "zero-plugin-api/plugin.toml";
  ok = writeManifest(zeroPluginApiPath, "id = \"me/zero-api\"\nname = \"Zero API\"\nplugin_api = 0\n") && ok;
  error.clear();
  const auto zeroPluginApi = scripting::parsePluginManifest(zeroPluginApiPath, &error);
  ok = expect(!zeroPluginApi.has_value(), "zero plugin_api should fail") && ok;
  ok = expectEq(error, "invalid 'plugin_api' (expected a positive integer)", "zero plugin API error") && ok;

  const auto oldApiDismissPath = root / "old-api-dismiss/plugin.toml";
  ok = writeManifest(
           oldApiDismissPath,
           "id = \"me/old-api-dismiss\"\n"
           "name = \"Old API Dismiss\"\n"
           "plugin_api = 7\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "dismiss_on_outside_click = false\n"
       )
      && ok;
  error.clear();
  const auto oldApiDismiss = scripting::parsePluginManifest(oldApiDismissPath, &error);
  ok = expect(!oldApiDismiss.has_value(), "dismiss_on_outside_click should require plugin API 8") && ok;
  ok = expectEq(
           error, "panel entry 'panel': dismiss_on_outside_click requires plugin_api >= 8",
           "dismiss outside-click API gate error"
       )
      && ok;

  const auto dismissPanelPath = root / "dismiss-panel/plugin.toml";
  ok = writeManifest(
           dismissPanelPath,
           "id = \"me/dismiss-panel\"\n"
           "name = \"Dismiss Panel\"\n"
           "plugin_api = 8\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "dismiss_on_outside_click = false\n"
       )
      && ok;
  error.clear();
  const auto dismissPanel = scripting::parsePluginManifest(dismissPanelPath, &error);
  ok = expect(dismissPanel.has_value(), error.empty() ? "failed to parse dismiss panel manifest" : error.c_str()) && ok;
  if (dismissPanel.has_value() && expect(dismissPanel->entries.size() == 1, "one dismiss panel entry expected")) {
    ok =
        expect(!dismissPanel->entries.front().panelDismissOnOutsideClick, "dismiss_on_outside_click false should parse")
        && ok;
  }

  const auto oldApiKeyboardPath = root / "old-api-keyboard/plugin.toml";
  ok = writeManifest(
           oldApiKeyboardPath,
           "id = \"me/old-api-keyboard\"\n"
           "name = \"Old API Keyboard\"\n"
           "plugin_api = 9\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "keyboard_focus = \"none\"\n"
       )
      && ok;
  error.clear();
  const auto oldApiKeyboard = scripting::parsePluginManifest(oldApiKeyboardPath, &error);
  ok = expect(!oldApiKeyboard.has_value(), "keyboard_focus should require plugin API 10") && ok;
  ok = expectEq(error, "panel entry 'panel': keyboard_focus requires plugin_api >= 10", "keyboard focus API gate error")
      && ok;

  const auto badKeyboardPath = root / "bad-keyboard/plugin.toml";
  ok = writeManifest(
           badKeyboardPath,
           "id = \"me/bad-keyboard\"\n"
           "name = \"Bad Keyboard\"\n"
           "plugin_api = 10\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "keyboard_focus = \"sometimes\"\n"
       )
      && ok;
  error.clear();
  const auto badKeyboard = scripting::parsePluginManifest(badKeyboardPath, &error);
  ok = expect(!badKeyboard.has_value(), "unknown keyboard_focus token should fail") && ok;
  ok = expectEq(
           error, R"(panel entry 'panel': keyboard_focus must be "on_demand", "exclusive" or "none")",
           "keyboard focus token error"
       )
      && ok;

  const auto keyboardDismissPath = root / "keyboard-dismiss/plugin.toml";
  ok = writeManifest(
           keyboardDismissPath,
           "id = \"me/keyboard-dismiss\"\n"
           "name = \"Keyboard Dismiss\"\n"
           "plugin_api = 10\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "keyboard_focus = \"none\"\n"
       )
      && ok;
  error.clear();
  const auto keyboardDismiss = scripting::parsePluginManifest(keyboardDismissPath, &error);
  ok = expect(!keyboardDismiss.has_value(), R"(keyboard_focus "none" should require dismiss_on_outside_click false)")
      && ok;
  ok = expectEq(
           error, R"(panel entry 'panel': keyboard_focus = "none" requires dismiss_on_outside_click = false)",
           "keyboard focus dismiss pairing error"
       )
      && ok;

  const auto keyboardPanelPath = root / "keyboard-panel/plugin.toml";
  ok = writeManifest(
           keyboardPanelPath,
           "id = \"me/keyboard-panel\"\n"
           "name = \"Keyboard Panel\"\n"
           "plugin_api = 10\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "keyboard_focus = \"none\"\n"
           "dismiss_on_outside_click = false\n"
       )
      && ok;
  error.clear();
  const auto keyboardPanel = scripting::parsePluginManifest(keyboardPanelPath, &error);
  ok = expect(keyboardPanel.has_value(), error.empty() ? "failed to parse keyboard panel manifest" : error.c_str())
      && ok;
  if (keyboardPanel.has_value() && expect(keyboardPanel->entries.size() == 1, "one keyboard panel entry expected")) {
    ok = expectEq(keyboardPanel->entries.front().panelKeyboardFocus, "none", "keyboard_focus none should parse") && ok;
  }

  const auto defaultKeyboardPath = root / "default-keyboard/plugin.toml";
  ok = writeManifest(
           defaultKeyboardPath,
           "id = \"me/default-keyboard\"\n"
           "name = \"Default Keyboard\"\n"
           "plugin_api = 10\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
       )
      && ok;
  error.clear();
  const auto defaultKeyboard = scripting::parsePluginManifest(defaultKeyboardPath, &error);
  ok = expect(defaultKeyboard.has_value(), error.empty() ? "failed to parse default keyboard manifest" : error.c_str())
      && ok;
  if (defaultKeyboard.has_value()
      && expect(defaultKeyboard->entries.size() == 1, "one default keyboard entry expected")) {
    ok = expectEq(
             defaultKeyboard->entries.front().panelKeyboardFocus, "on_demand", "keyboard_focus defaults to on_demand"
         )
        && ok;
  }

  const auto oldApiPersistentPath = root / "old-api-persistent/plugin.toml";
  ok = writeManifest(
           oldApiPersistentPath,
           "id = \"me/old-api-persistent\"\n"
           "name = \"Old API Persistent\"\n"
           "plugin_api = 10\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "persistent = true\n"
       )
      && ok;
  error.clear();
  const auto oldApiPersistent = scripting::parsePluginManifest(oldApiPersistentPath, &error);
  ok = expect(!oldApiPersistent.has_value(), "persistent should require plugin API 11") && ok;
  ok = expectEq(error, "panel entry 'panel': persistent requires plugin_api >= 11", "persistent API gate error") && ok;

  const auto persistentDismissPath = root / "persistent-dismiss/plugin.toml";
  ok = writeManifest(
           persistentDismissPath,
           "id = \"me/persistent-dismiss\"\n"
           "name = \"Persistent Dismiss\"\n"
           "plugin_api = 11\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "persistent = true\n"
       )
      && ok;
  error.clear();
  const auto persistentDismiss = scripting::parsePluginManifest(persistentDismissPath, &error);
  ok = expect(!persistentDismiss.has_value(), "persistent should require dismiss_on_outside_click false") && ok;
  ok = expectEq(
           error, "panel entry 'panel': persistent = true requires dismiss_on_outside_click = false",
           "persistent dismiss pairing error"
       )
      && ok;

  const auto persistentExclusivePath = root / "persistent-exclusive/plugin.toml";
  ok = writeManifest(
           persistentExclusivePath,
           "id = \"me/persistent-exclusive\"\n"
           "name = \"Persistent Exclusive\"\n"
           "plugin_api = 11\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "keyboard_focus = \"exclusive\"\n"
           "dismiss_on_outside_click = false\n"
           "persistent = true\n"
       )
      && ok;
  error.clear();
  const auto persistentExclusive = scripting::parsePluginManifest(persistentExclusivePath, &error);
  ok = expect(!persistentExclusive.has_value(), "persistent should reject exclusive keyboard focus") && ok;
  ok = expectEq(
           error, R"(panel entry 'panel': persistent = true is incompatible with keyboard_focus = "exclusive")",
           "persistent exclusive keyboard error"
       )
      && ok;

  const auto persistentAttachedPath = root / "persistent-attached/plugin.toml";
  ok = writeManifest(
           persistentAttachedPath,
           "id = \"me/persistent-attached\"\n"
           "name = \"Persistent Attached\"\n"
           "plugin_api = 11\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "placement = \"attached\"\n"
           "dismiss_on_outside_click = false\n"
           "persistent = true\n"
       )
      && ok;
  error.clear();
  const auto persistentAttached = scripting::parsePluginManifest(persistentAttachedPath, &error);
  ok = expect(!persistentAttached.has_value(), "persistent should reject attached placement") && ok;
  ok = expectEq(
           error, R"(panel entry 'panel': persistent = true requires placement = "floating")",
           "persistent attached placement error"
       )
      && ok;

  const auto oskPanelPath = root / "osk-panel/plugin.toml";
  ok = writeManifest(
           oskPanelPath,
           "id = \"me/osk-panel\"\n"
           "name = \"OSK Panel\"\n"
           "plugin_api = 11\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "width = \"fill\"\n"
           "position = \"bottom_center\"\n"
           "keyboard_focus = \"none\"\n"
           "dismiss_on_outside_click = false\n"
           "persistent = true\n"
       )
      && ok;
  error.clear();
  const auto oskPanel = scripting::parsePluginManifest(oskPanelPath, &error);
  ok = expect(oskPanel.has_value(), error.empty() ? "failed to parse osk panel manifest" : error.c_str()) && ok;
  if (oskPanel.has_value() && expect(oskPanel->entries.size() == 1, "one osk panel entry expected")) {
    const auto& entry = oskPanel->entries.front();
    ok = expect(entry.panelPersistent, "persistent true should parse") && ok;
    ok = expectEq(entry.panelKeyboardFocus, "none", "osk keyboard_focus should parse") && ok;
    ok = expect(entry.panelWidthFill, "osk width fill should parse") && ok;
    // A persistent panel gets no placement / open_near_click settings: it is always
    // floating and never opened from a bar widget's anchor.
    const bool hasPlacement = std::ranges::any_of(entry.settings, [](const scripting::ManifestField& field) {
      return field.key == "panel_placement";
    });
    const bool hasPosition = std::ranges::any_of(entry.settings, [](const scripting::ManifestField& field) {
      return field.key == "panel_position";
    });
    ok = expect(!hasPlacement, "persistent panel should not seed a placement setting") && ok;
    ok = expect(hasPosition, "persistent panel should still seed a position setting") && ok;
  }

  const auto oldApiCapturePath = root / "old-api-capture/plugin.toml";
  ok = writeManifest(
           oldApiCapturePath,
           "id = \"me/old-api-capture\"\n"
           "name = \"Old API Capture\"\n"
           "plugin_api = 12\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "capture_keys = [\"space\"]\n"
       )
      && ok;
  error.clear();
  ok = expect(
           !scripting::parsePluginManifest(oldApiCapturePath, &error).has_value(),
           "capture_keys should require plugin API 13"
       )
      && ok;
  ok = expectEq(error, "panel entry 'panel': capture_keys requires plugin_api >= 13", "capture_keys API gate error")
      && ok;

  const auto badCapturePath = root / "bad-capture/plugin.toml";
  ok = writeManifest(
           badCapturePath,
           "id = \"me/bad-capture\"\n"
           "name = \"Bad Capture\"\n"
           "plugin_api = 13\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "capture_keys = [\"space\", \"nonsensekey\"]\n"
       )
      && ok;
  error.clear();
  ok = expect(!scripting::parsePluginManifest(badCapturePath, &error).has_value(), "an invalid key chord should fail")
      && ok;
  ok = expectEq(
           error, "panel entry 'panel': capture_keys entry 'nonsensekey' is not a valid key chord",
           "capture_keys chord error"
       )
      && ok;

  // A Super chord belongs to the compositor; parseKeyChordSpec throws rather than returning
  // nullopt for it, so this covers the other rejection path.
  const auto superCapturePath = root / "super-capture/plugin.toml";
  ok = writeManifest(
           superCapturePath,
           "id = \"me/super-capture\"\n"
           "name = \"Super Capture\"\n"
           "plugin_api = 13\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "capture_keys = [\"super+space\"]\n"
       )
      && ok;
  error.clear();
  ok = expect(!scripting::parsePluginManifest(superCapturePath, &error).has_value(), "a Super chord should be rejected")
      && ok;
  ok = expect(
           error.starts_with("panel entry 'panel': capture_keys entry 'super+space': "),
           "Super chord error should name the entry and spec"
       )
      && ok;

  const auto captureNoFocusPath = root / "capture-no-focus/plugin.toml";
  ok = writeManifest(
           captureNoFocusPath,
           "id = \"me/capture-no-focus\"\n"
           "name = \"Capture No Focus\"\n"
           "plugin_api = 13\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "keyboard_focus = \"none\"\n"
           "dismiss_on_outside_click = false\n"
           "capture_keys = [\"space\"]\n"
       )
      && ok;
  error.clear();
  ok = expect(
           !scripting::parsePluginManifest(captureNoFocusPath, &error).has_value(),
           R"(capture_keys should be rejected with keyboard_focus "none")"
       )
      && ok;
  ok = expectEq(
           error, R"(panel entry 'panel': capture_keys requires keyboard_focus "on_demand" or "exclusive")",
           "capture_keys focus pairing error"
       )
      && ok;

  const auto capturePath = root / "capture/plugin.toml";
  ok = writeManifest(
           capturePath,
           "id = \"me/capture\"\n"
           "name = \"Capture\"\n"
           "plugin_api = 13\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "capture_keys = [\"space\", \"ctrl+r\"]\n"
       )
      && ok;
  error.clear();
  const auto capture = scripting::parsePluginManifest(capturePath, &error);
  ok = expect(capture.has_value(), error.empty() ? "valid capture_keys should parse" : error.c_str()) && ok;
  if (capture.has_value() && !capture->entries.empty()) {
    const auto& keys = capture->entries.front().panelCaptureKeys;
    ok = expect(keys.size() == 2, "both capture_keys entries should parse") && ok;
    if (keys.size() == 2) {
      // Stored verbatim: the script is called back with the exact spec it declared.
      ok = expectEq(keys[0], "space", "first capture_keys entry") && ok;
      ok = expectEq(keys[1], "ctrl+r", "second capture_keys entry") && ok;
    }
  }

  const auto noCapturePath = root / "no-capture/plugin.toml";
  ok = writeManifest(
           noCapturePath,
           "id = \"me/no-capture\"\n"
           "name = \"No Capture\"\n"
           "plugin_api = 13\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
       )
      && ok;
  error.clear();
  const auto noCapture = scripting::parsePluginManifest(noCapturePath, &error);
  ok = expect(noCapture.has_value(), "a panel without capture_keys should parse") && ok;
  if (noCapture.has_value() && !noCapture->entries.empty()) {
    const auto& entry = noCapture->entries.front();
    ok = expect(entry.panelCaptureKeys.empty(), "capture_keys should default to empty") && ok;
    ok = expectEq(entry.panelLayerDefault, "top", "layer should default to top") && ok;
    const auto layerField = std::ranges::find(entry.settings, "panel_layer", &scripting::ManifestField::key);
    ok = expect(layerField != entry.settings.end(), "panel layer setting should be injected") && ok;
    if (layerField != entry.settings.end()) {
      ok = expectEq(layerField->stringDefault, "top", "injected layer setting should default to top") && ok;
    }
  }

  const auto oldApiLayerPath = root / "old-api-layer/plugin.toml";
  ok = writeManifest(
           oldApiLayerPath,
           "id = \"me/old-api-layer\"\n"
           "name = \"Old API Layer\"\n"
           "plugin_api = 29\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "layer = \"overlay\"\n"
       )
      && ok;
  error.clear();
  ok = expect(
           !scripting::parsePluginManifest(oldApiLayerPath, &error).has_value(),
           "layer should require its plugin API level"
       )
      && ok;
  ok = expectEq(error, "panel entry 'panel': layer requires plugin_api >= 30", "layer api gate error") && ok;

  const auto badLayerPath = root / "bad-layer/plugin.toml";
  ok = writeManifest(
           badLayerPath,
           "id = \"me/bad-layer\"\n"
           "name = \"Bad Layer\"\n"
           "plugin_api = 30\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "layer = \"bottom\"\n"
       )
      && ok;
  error.clear();
  ok = expect(!scripting::parsePluginManifest(badLayerPath, &error).has_value(), "an unknown layer should be rejected")
      && ok;
  ok = expectEq(error, R"(panel entry 'panel': layer must be "top" or "overlay")", "layer vocabulary error") && ok;

  const auto overlayLayerPath = root / "overlay-layer/plugin.toml";
  ok = writeManifest(
           overlayLayerPath,
           "id = \"me/overlay-layer\"\n"
           "name = \"Overlay Layer\"\n"
           "plugin_api = 30\n"
           "[[panel]]\n"
           "id = \"panel\"\n"
           "entry = \"panel.luau\"\n"
           "layer = \"overlay\"\n"
       )
      && ok;
  error.clear();
  const auto overlayLayer = scripting::parsePluginManifest(overlayLayerPath, &error);
  ok = expect(overlayLayer.has_value(), error.empty() ? "a valid layer should parse" : error.c_str()) && ok;
  if (overlayLayer.has_value() && !overlayLayer->entries.empty()) {
    const auto& entry = overlayLayer->entries.front();
    ok = expectEq(entry.panelLayerDefault, "overlay", "layer default should parse") && ok;
    const auto layerField = std::ranges::find(entry.settings, "panel_layer", &scripting::ManifestField::key);
    ok = expect(layerField != entry.settings.end(), "panel layer setting should be injected") && ok;
    if (layerField != entry.settings.end()) {
      ok = expectEq(layerField->stringDefault, "overlay", "manifest layer should seed the user setting") && ok;
    }
    const auto settings = scripting::seedEntrySettings(entry, {{"panel_layer", std::string("top")}});
    const auto shellConfig = scripting::resolvePluginPanelShellConfig(entry, settings);
    ok = expectEq(shellConfig.layer, "top", "user layer override should win") && ok;
  }

  // A [[widget]] entry can declare bar gesture defaults, kept as raw strings: the gesture
  // vocabulary and the action grammar belong to the bar, not to the manifest parser.
  const auto actionsPath = root / "actions" / "plugin.toml";
  ok = expect(
           writeManifest(
               actionsPath,
               "id = \"me/actions\"\n"
               "name = \"Actions\"\n"
               "plugin_api = 14\n"
               "[[widget]]\n"
               "id = \"bar\"\n"
               "entry = \"bar.luau\"\n"
               "[widget.actions]\n"
               "middle = \"exec playerctl pause\"\n"
               "scroll_up = \"volume-up\"\n"
           ) && !actionsPath.empty(),
           "failed to write the actions manifest"
       )
      && ok;
  error.clear();
  const auto actions = scripting::parsePluginManifest(actionsPath, &error);
  ok = expect(actions.has_value(), error.empty() ? "a widget with actions should parse" : error.c_str()) && ok;
  if (actions.has_value() && !actions->entries.empty()) {
    const auto& declared = actions->entries.front().widgetActions;
    ok = expect(declared.size() == 2, "both declared actions should survive") && ok;
    const auto middle = std::ranges::find(declared, "middle", &std::pair<std::string, std::string>::first);
    ok = expect(middle != declared.end(), "the middle action should be recorded") && ok;
    if (middle != declared.end()) {
      ok = expectEq(middle->second, "exec playerctl pause", "the middle action should keep its command verbatim") && ok;
    }
  }

  // The capability is gated on its API level, like every other manifest addition.
  const auto oldApiPath = root / "old-api" / "plugin.toml";
  ok = expect(
           writeManifest(
               oldApiPath,
               "id = \"me/old-api\"\n"
               "name = \"Old Api\"\n"
               "plugin_api = 13\n"
               "[[widget]]\n"
               "id = \"bar\"\n"
               "entry = \"bar.luau\"\n"
               "[widget.actions]\n"
               "middle = \"volume-mute\"\n"
           ),
           "failed to write the old api manifest"
       )
      && ok;
  error.clear();
  ok = expect(
           !scripting::parsePluginManifest(oldApiPath, &error).has_value(),
           "actions below its plugin_api level should fail"
       )
      && ok;

  // A non-string action is a manifest error rather than something to guess at.
  const auto badActionPath = root / "bad-action" / "plugin.toml";
  ok = expect(
           writeManifest(
               badActionPath,
               "id = \"me/bad-action\"\n"
               "name = \"Bad Action\"\n"
               "plugin_api = 14\n"
               "[[widget]]\n"
               "id = \"bar\"\n"
               "entry = \"bar.luau\"\n"
               "[widget.actions]\n"
               "middle = 42\n"
           ),
           "failed to write the bad action manifest"
       )
      && ok;
  error.clear();
  ok = expect(!scripting::parsePluginManifest(badActionPath, &error).has_value(), "a non-string action should fail")
      && ok;

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  return ok ? 0 : 1;
}
