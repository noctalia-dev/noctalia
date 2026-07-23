#include "scripting/plugin_api.h"
#include "scripting/plugin_manifest.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <print>
#include <string>
#include <string_view>
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
  ok = writeText(defaultManifestPath, "id = \"me/defaults\"\nname = \"Defaults\"\nplugin_api = 3\n") && ok;

  std::string error;
  const auto defaults = scripting::parsePluginManifest(defaultManifestPath, &error);
  ok = expect(defaults.has_value(), error.empty() ? "failed to parse default manifest" : error.c_str()) && ok;
  if (defaults.has_value()) {
    ok = expect(defaults->pluginApiVersion == 3, "plugin API version should parse") && ok;
    ok = expectEq(defaults->license, "MIT", "license should default to MIT") && ok;
    ok = expect(!defaults->deprecated, "deprecated should default to false") && ok;
    ok = expect(defaults->dependencies.empty(), "dependencies should default to empty") && ok;
  }

  const auto explicitManifestPath = root / "explicit/plugin.toml";
  ok = writeText(
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
  ok = writeText(
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

  const auto literalLabelPath = root / "literal-label/plugin.toml";
  ok = writeText(
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
  ok = writeText(
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
  ok = writeText(
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
  ok = writeText(
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
  ok = writeText(
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
  ok = writeText(
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
  ok = writeText(
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
  ok = writeText(
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
  ok = writeText(
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
  ok = writeText(
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
  ok = writeText(
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

  // Panel width/height: number, "fill", or a loud error — never a silent default.
  const auto fillPanelManifestPath = root / "fill-panel/plugin.toml";
  ok = writeText(
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
  ok = writeText(
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
  ok = writeText(
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
  ok = writeText(
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
  ok = writeText(missingNameManifestPath, "id = \"me/missing-name\"\nplugin_api = 3\n") && ok;
  error.clear();
  const auto missingName = scripting::parsePluginManifest(missingNameManifestPath, &error);
  ok = expect(!missingName.has_value(), "manifest without name should fail") && ok;
  ok = expectEq(error, "missing mandatory key 'name'", "missing name error") && ok;

  const auto missingPluginApiPath = root / "missing-plugin-api/plugin.toml";
  ok = writeText(missingPluginApiPath, "id = \"me/missing-api\"\nname = \"Missing API\"\n") && ok;
  error.clear();
  const auto missingPluginApi = scripting::parsePluginManifest(missingPluginApiPath, &error);
  ok = expect(!missingPluginApi.has_value(), "manifest without plugin_api should fail") && ok;
  ok = expectEq(error, "missing mandatory key 'plugin_api'", "missing plugin API error") && ok;

  const auto invalidPluginApiPath = root / "invalid-plugin-api/plugin.toml";
  ok = writeText(invalidPluginApiPath, "id = \"me/invalid-api\"\nname = \"Invalid API\"\nplugin_api = \"3\"\n") && ok;
  error.clear();
  const auto invalidPluginApi = scripting::parsePluginManifest(invalidPluginApiPath, &error);
  ok = expect(!invalidPluginApi.has_value(), "string plugin_api should fail") && ok;
  ok = expectEq(error, "invalid 'plugin_api' (expected a positive integer)", "invalid plugin API error") && ok;

  const auto zeroPluginApiPath = root / "zero-plugin-api/plugin.toml";
  ok = writeText(zeroPluginApiPath, "id = \"me/zero-api\"\nname = \"Zero API\"\nplugin_api = 0\n") && ok;
  error.clear();
  const auto zeroPluginApi = scripting::parsePluginManifest(zeroPluginApiPath, &error);
  ok = expect(!zeroPluginApi.has_value(), "zero plugin_api should fail") && ok;
  ok = expectEq(error, "invalid 'plugin_api' (expected a positive integer)", "zero plugin API error") && ok;

  const auto oldApiDismissPath = root / "old-api-dismiss/plugin.toml";
  ok = writeText(
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
           error,
           "panel entry 'panel': dismiss_on_outside_click requires plugin_api >= 8",
           "dismiss outside-click API gate error"
       )
      && ok;

  const auto dismissPanelPath = root / "dismiss-panel/plugin.toml";
  ok = writeText(
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
  ok = expect(dismissPanel.has_value(), error.empty() ? "failed to parse dismiss panel manifest" : error.c_str())
      && ok;
  if (dismissPanel.has_value() && expect(dismissPanel->entries.size() == 1, "one dismiss panel entry expected")) {
    ok = expect(!dismissPanel->entries.front().panelDismissOnOutsideClick, "dismiss_on_outside_click false should parse")
        && ok;
  }

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  return ok ? 0 : 1;
}
