#include "scripting/plugin_api.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_panel_shell.h"
#include "tests/test_check.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

  std::filesystem::path g_root;

  // Runs at exit so a failed check still removes the fixture directory.
  void removeRoot() {
    std::error_code ec;
    std::filesystem::remove_all(g_root, ec);
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
  g_root = makeTempDir();
  TEST_CHECK(!g_root.empty());
  std::atexit(removeRoot);
  const auto& root = g_root;

  static_assert(scripting::kOldestSupportedPluginApiVersion > 0);
  TEST_CHECK(!scripting::supportsPluginApiVersion(scripting::kOldestSupportedPluginApiVersion - 1));
  TEST_CHECK(scripting::supportsPluginApiVersion(scripting::kOldestSupportedPluginApiVersion));
  TEST_CHECK(scripting::supportsPluginApiVersion(scripting::kCurrentPluginApiVersion));
  if constexpr (scripting::kCurrentPluginApiVersion < std::numeric_limits<std::uint32_t>::max()) {
    TEST_CHECK(!scripting::supportsPluginApiVersion(scripting::kCurrentPluginApiVersion + 1));
  }
  const auto defaultManifestPath = root / "defaults/plugin.toml";
  TEST_CHECK(writeManifest(defaultManifestPath, "id = \"me/defaults\"\nname = \"Defaults\"\nplugin_api = 3\n"));

  std::string error;
  const auto defaults = scripting::parsePluginManifest(defaultManifestPath, &error);
  TEST_CHECK(defaults.has_value());
  TEST_CHECK(defaults->pluginApiVersion == 3);
  TEST_CHECK(defaults->license == "MIT");
  TEST_CHECK(!defaults->deprecated);
  TEST_CHECK(defaults->dependencies.empty());
  TEST_CHECK(scripting::isValidPluginVersion("0.0.0"));
  TEST_CHECK(scripting::isValidPluginVersion("10.20.30"));
  TEST_CHECK(!scripting::isValidPluginVersion("1.2"));
  TEST_CHECK(!scripting::isValidPluginVersion("1.2.3.4"));
  TEST_CHECK(!scripting::isValidPluginVersion("01.2.3"));
  TEST_CHECK(!scripting::isValidPluginVersion("1.02.3"));
  TEST_CHECK(!scripting::isValidPluginVersion("1.2.03"));
  TEST_CHECK(!scripting::isValidPluginVersion("1.2.3-beta.1"));
  TEST_CHECK(!scripting::isValidPluginVersion("1.2.three"));

  const auto missingVersionPath = root / "missing-version/plugin.toml";
  TEST_CHECK(
      writeText(missingVersionPath, "id = \"me/missing-version\"\nname = \"Missing Version\"\nplugin_api = 3\n")
  );
  error.clear();
  const auto missingVersion = scripting::parsePluginManifest(missingVersionPath, &error);
  TEST_CHECK(!missingVersion.has_value());
  TEST_CHECK(error == "missing mandatory key 'version'");

  const auto invalidVersionPath = root / "invalid-version/plugin.toml";
  TEST_CHECK(writeText(
      invalidVersionPath,
      "id = \"me/invalid-version\"\n"
      "name = \"Invalid Version\"\n"
      "version = \"1.2.3-beta.1\"\n"
      "plugin_api = 3\n"
  ));
  error.clear();
  const auto invalidVersion = scripting::parsePluginManifest(invalidVersionPath, &error);
  TEST_CHECK(!invalidVersion.has_value());
  TEST_CHECK(error == "invalid 'version' (expected MAJOR.MINOR.PATCH)");

  const auto explicitManifestPath = root / "explicit/plugin.toml";
  TEST_CHECK(writeManifest(
      explicitManifestPath,
      "id = \"me/explicit\"\n"
      "name = \"Explicit\"\n"
      "plugin_api = 3\n"
      "license = \"Apache-2.0\"\n"
      "deprecated = true\n"
      "dependencies = [\"grim\", \"slurp\"]\n"
  ));

  error.clear();
  const auto explicitManifest = scripting::parsePluginManifest(explicitManifestPath, &error);
  TEST_CHECK(explicitManifest.has_value());
  TEST_CHECK(explicitManifest->license == "Apache-2.0");
  TEST_CHECK(explicitManifest->deprecated);
  TEST_CHECK(explicitManifest->dependencies.size() == 2);
  TEST_CHECK(explicitManifest->dependencies[0] == "grim");
  TEST_CHECK(explicitManifest->dependencies[1] == "slurp");

  const auto translatedSettingsManifestPath = root / "translated-settings/plugin.toml";
  TEST_CHECK(writeManifest(
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
  ));
  error.clear();
  const auto translatedSettingsManifest = scripting::parsePluginManifest(translatedSettingsManifestPath, &error);
  TEST_CHECK(translatedSettingsManifest.has_value());
  {
    TEST_CHECK(translatedSettingsManifest->settings.size() == 1);
    const auto& setting = translatedSettingsManifest->settings.front();
    TEST_CHECK(setting.labelKey == "settings.mode.label");
    TEST_CHECK(setting.descriptionKey == "settings.mode.description");
    TEST_CHECK(setting.options.size() == 2);
    TEST_CHECK(setting.options[0].labelKey == "settings.mode.options.auto");
    TEST_CHECK(setting.options[1].labelKey == "settings.mode.options.manual");
    TEST_CHECK(translatedSettingsManifest->entries.size() == 1);
    const auto& settings = translatedSettingsManifest->entries.front().settings;
    TEST_CHECK(settings.size() == 1);
    TEST_CHECK(settings.front().labelKey == "settings.label.label");
  }

  const auto pathSettingsManifestPath = root / "path-settings/plugin.toml";
  TEST_CHECK(writeManifest(
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
  ));
  error.clear();
  const auto pathSettingsManifest = scripting::parsePluginManifest(pathSettingsManifestPath, &error);
  TEST_CHECK(pathSettingsManifest.has_value());
  {
    TEST_CHECK(pathSettingsManifest->settings.size() == 1);
    const auto& fileSetting = pathSettingsManifest->settings.front();
    TEST_CHECK(fileSetting.type == scripting::ManifestFieldType::File);
    TEST_CHECK(fileSetting.extensions.size() == 2);
    TEST_CHECK(pathSettingsManifest->entries.size() == 1);
    TEST_CHECK(pathSettingsManifest->entries.front().settings.size() == 1);
    TEST_CHECK(pathSettingsManifest->entries.front().settings.front().type == scripting::ManifestFieldType::Folder);
  }

  const auto literalLabelPath = root / "literal-label/plugin.toml";
  TEST_CHECK(writeManifest(
      literalLabelPath,
      "id = \"me/literal-label\"\n"
      "name = \"Literal Label\"\n"
      "plugin_api = 3\n"
      "[[setting]]\n"
      "key = \"mode\"\n"
      "label = \"Mode\"\n"
  ));
  error.clear();
  const auto literalLabel = scripting::parsePluginManifest(literalLabelPath, &error);
  TEST_CHECK(!literalLabel.has_value());
  TEST_CHECK(error == "setting 'mode' uses 'label'; use 'label_key' that points to translation key instead");

  const auto literalDescriptionPath = root / "literal-description/plugin.toml";
  TEST_CHECK(writeManifest(
      literalDescriptionPath,
      "id = \"me/literal-description\"\n"
      "name = \"Literal Description\"\n"
      "plugin_api = 3\n"
      "[[setting]]\n"
      "key = \"mode\"\n"
      "label_key = \"settings.mode.label\"\n"
      "description = \"Mode\"\n"
  ));
  error.clear();
  const auto literalDescription = scripting::parsePluginManifest(literalDescriptionPath, &error);
  TEST_CHECK(!literalDescription.has_value());
  TEST_CHECK(
      error == "setting 'mode' uses 'description'; use 'description_key' that points to translation key instead"
  );

  const auto missingLabelKeyPath = root / "missing-label-key/plugin.toml";
  TEST_CHECK(writeManifest(
      missingLabelKeyPath,
      "id = \"me/missing-label-key\"\n"
      "name = \"Missing Label Key\"\n"
      "plugin_api = 3\n"
      "[[setting]]\n"
      "key = \"mode\"\n"
  ));
  error.clear();
  const auto missingLabelKey = scripting::parsePluginManifest(missingLabelKeyPath, &error);
  TEST_CHECK(!missingLabelKey.has_value());
  TEST_CHECK(error == "setting 'mode' is missing 'label_key'");

  const auto literalOptionLabelPath = root / "literal-option-label/plugin.toml";
  TEST_CHECK(writeManifest(
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
  ));
  error.clear();
  const auto literalOptionLabel = scripting::parsePluginManifest(literalOptionLabelPath, &error);
  TEST_CHECK(!literalOptionLabel.has_value());
  TEST_CHECK(
      error == "setting 'mode' option 'auto' uses 'label'; use 'label_key' that points to translation key instead"
  );

  const auto missingOptionLabelKeyPath = root / "missing-option-label-key/plugin.toml";
  TEST_CHECK(writeManifest(
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
  ));
  error.clear();
  const auto missingOptionLabelKey = scripting::parsePluginManifest(missingOptionLabelKeyPath, &error);
  TEST_CHECK(!missingOptionLabelKey.has_value());
  TEST_CHECK(error == "setting 'mode' option must be a table with value and label_key");

  const auto launcherManifestPath = root / "launcher/plugin.toml";
  TEST_CHECK(writeManifest(
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
  ));
  error.clear();
  const auto launcherManifest = scripting::parsePluginManifest(launcherManifestPath, &error);
  TEST_CHECK(launcherManifest.has_value());
  TEST_CHECK(launcherManifest->entries.size() == 1);
  {
    const auto& entry = launcherManifest->entries.front();
    TEST_CHECK(entry.kind == scripting::PluginEntryKind::LauncherProvider);
    TEST_CHECK(entry.launcherPrefix == ":tr");
    TEST_CHECK(entry.launcherGlyph == "language");
    TEST_CHECK(entry.launcherGlobalSearch);
    TEST_CHECK(entry.launcherDebounceMs == 300);
    TEST_CHECK(entry.launcherCategories.size() == 1);
    TEST_CHECK(entry.launcherCategories.front().label == "Languages");
    TEST_CHECK(entry.launcherCategories.front().glyph == "world");
  }

  // Entry-level settings on a launcher provider (a singleton with no settings UI)
  // are rejected — authors must use a plugin-level [[setting]] instead.
  const auto launcherSettingManifestPath = root / "launcher-setting/plugin.toml";
  TEST_CHECK(writeManifest(
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
  ));
  error.clear();
  const auto launcherSettingManifest = scripting::parsePluginManifest(launcherSettingManifestPath, &error);
  TEST_CHECK(!launcherSettingManifest.has_value());
  TEST_CHECK(
      error
      == "entry 'translate' of kind 'launcher_provider' declares [[launcher_provider.setting]], but entry-level "
         "settings are only supported for widget, desktop_widget, and panel entries; move it to a plugin-level "
         "[[setting]]"
  );

  const auto listManifestPath = root / "string-list/plugin.toml";
  TEST_CHECK(writeManifest(
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
  ));
  error.clear();
  const auto listManifest = scripting::parsePluginManifest(listManifestPath, &error);
  TEST_CHECK(listManifest.has_value());
  TEST_CHECK(listManifest->entries.size() == 1);
  {
    const auto& settings = listManifest->entries.front().settings;
    TEST_CHECK(settings.size() == 1);
    TEST_CHECK(settings.front().type == scripting::ManifestFieldType::StringList);
    const auto defaultValue = settings.front().defaultValue();
    const auto* values = std::get_if<std::vector<std::string>>(&defaultValue);
    TEST_CHECK(values != nullptr);
    TEST_CHECK(values->size() == 2);
    TEST_CHECK((*values)[0] == "/dev/input/by-id/a");
    TEST_CHECK((*values)[1] == "/dev/input/by-path/b");
  }

  const auto mapManifestPath = root / "string-map/plugin.toml";
  TEST_CHECK(writeManifest(
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
  ));
  error.clear();
  const auto mapManifest = scripting::parsePluginManifest(mapManifestPath, &error);
  TEST_CHECK(mapManifest.has_value());
  TEST_CHECK(mapManifest->entries.size() == 1);
  {
    const auto& settings = mapManifest->entries.front().settings;
    TEST_CHECK(settings.size() == 1);
    TEST_CHECK(settings.front().type == scripting::ManifestFieldType::StringMap);
    const auto defaultValue = settings.front().defaultValue();
    const auto* values = std::get_if<WidgetSettingStringMap>(&defaultValue);
    TEST_CHECK(values != nullptr);
    TEST_CHECK(values->size() == 2);
    TEST_CHECK(values->at("eDP-1") == "laptop");
    TEST_CHECK(values->at("DP-1") == "monitor");
  }

  const auto invalidMapManifestPath = root / "invalid-string-map/plugin.toml";
  TEST_CHECK(writeManifest(
      invalidMapManifestPath,
      "id = \"me/invalid-string-map\"\n"
      "name = \"Invalid String Map\"\n"
      "plugin_api = 6\n"
      "[[setting]]\n"
      "key = \"output_glyphs\"\n"
      "type = \"string_map\"\n"
      "label_key = \"settings.output_glyphs.label\"\n"
      "default = { \"eDP-1\" = 1 }\n"
  ));
  error.clear();
  const auto invalidMapManifest = scripting::parsePluginManifest(invalidMapManifestPath, &error);
  TEST_CHECK(!invalidMapManifest.has_value());
  TEST_CHECK(error == "setting 'output_glyphs' string_map default values must be strings");

  const auto oldApiMapManifestPath = root / "old-api-string-map/plugin.toml";
  TEST_CHECK(writeManifest(
      oldApiMapManifestPath,
      "id = \"me/old-api-string-map\"\n"
      "name = \"Old API String Map\"\n"
      "plugin_api = 5\n"
      "[[setting]]\n"
      "key = \"output_glyphs\"\n"
      "type = \"string_map\"\n"
      "label_key = \"settings.output_glyphs.label\"\n"
      "default = {}\n"
  ));
  error.clear();
  const auto oldApiMapManifest = scripting::parsePluginManifest(oldApiMapManifestPath, &error);
  TEST_CHECK(!oldApiMapManifest.has_value());
  TEST_CHECK(error == "setting 'output_glyphs' type 'string_map' requires plugin_api >= 6");

  const auto doubleManifestPath = root / "double-setting/plugin.toml";
  TEST_CHECK(writeManifest(
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
  ));
  error.clear();
  const auto doubleManifest = scripting::parsePluginManifest(doubleManifestPath, &error);
  TEST_CHECK(doubleManifest.has_value());
  TEST_CHECK(doubleManifest->entries.size() == 1);
  {
    const auto& settings = doubleManifest->entries.front().settings;
    TEST_CHECK(settings.size() == 1);
    const auto& setting = settings.front();
    TEST_CHECK(setting.type == scripting::ManifestFieldType::Double);
    TEST_CHECK(setting.numberDefault == 0.5);
    TEST_CHECK(setting.minValue == 0.0);
    TEST_CHECK(setting.maxValue == 1.0);
    TEST_CHECK(std::abs(setting.step - 0.05) <= std::numeric_limits<double>::epsilon());
  }

  const auto checkInvalidNumericSetting = [&](std::string_view fixtureName, std::string_view settingBody,
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
    TEST_CHECK(writeManifest(manifestPath, manifest));
    error.clear();
    const auto parsed = scripting::parsePluginManifest(manifestPath, &error);
    TEST_CHECK(!parsed.has_value());
    TEST_CHECK(error == expectedError);
  };

  checkInvalidNumericSetting(
      "double-string-default", "type = \"double\"\ndefault = \"fast\"\n",
      "setting 'value' double default must be a finite number"
  );
  checkInvalidNumericSetting(
      "double-zero-step", "type = \"double\"\ndefault = 0.5\nstep = 0.0\n",
      "setting 'value' step must be greater than zero"
  );
  checkInvalidNumericSetting(
      "double-inverted-range", "type = \"double\"\ndefault = 0.5\nmin = 1.0\nmax = 0.0\n",
      "setting 'value' min must be less than or equal to max"
  );
  checkInvalidNumericSetting(
      "int-float-min", "type = \"int\"\ndefault = 2\nmin = 0.5\n", "setting 'value' min must be an integer"
  );
  checkInvalidNumericSetting(
      "string-numeric-bound", "type = \"string\"\ndefault = \"value\"\nmin = 0\n",
      "setting 'value' min is only valid for int or double"
  );

  // Panel width/height: number, "fill", or a loud error — never a silent default.
  const auto fillPanelManifestPath = root / "fill-panel/plugin.toml";
  TEST_CHECK(writeManifest(
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
  ));
  error.clear();
  const auto fillPanel = scripting::parsePluginManifest(fillPanelManifestPath, &error);
  TEST_CHECK(fillPanel.has_value());
  TEST_CHECK(fillPanel->entries.size() == 1);
  {
    const auto& entry = fillPanel->entries.front();
    TEST_CHECK(entry.panelWidth == 420.0);
    TEST_CHECK(!entry.panelWidthFill);
    TEST_CHECK(entry.panelHeightFill);
  }

  const auto badFillManifestPath = root / "bad-fill/plugin.toml";
  TEST_CHECK(writeManifest(
      badFillManifestPath,
      "id = \"me/bad-fill\"\n"
      "name = \"Bad Fill\"\n"
      "plugin_api = 3\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
      "height = \"full\"\n"
  ));
  error.clear();
  const auto badFill = scripting::parsePluginManifest(badFillManifestPath, &error);
  TEST_CHECK(!badFill.has_value());
  TEST_CHECK(error == R"(panel entry 'panel': height must be a positive number, "fill", or "content")");

  // height = "content" fits the panel to its tree, capped by the required max_height.
  const auto contentPanelManifestPath = root / "content-panel/plugin.toml";
  TEST_CHECK(writeManifest(
      contentPanelManifestPath,
      "id = \"me/content-panel\"\n"
      "name = \"Content Panel\"\n"
      "plugin_api = 42\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
      "width = 300\n"
      "height = \"content\"\n"
      "max_height = 600\n"
  ));
  error.clear();
  const auto contentPanel = scripting::parsePluginManifest(contentPanelManifestPath, &error);
  TEST_CHECK(contentPanel.has_value());
  TEST_CHECK(contentPanel->entries.size() == 1);
  {
    const auto& entry = contentPanel->entries.front();
    TEST_CHECK(entry.panelHeightContent);
    TEST_CHECK(!entry.panelHeightFill);
    TEST_CHECK(entry.panelMaxHeight == 600.0);
  }

  const auto contentErrors = std::vector<std::pair<std::string, std::string>>{
      {"plugin_api = 41\n[[panel]]\nid = \"panel\"\nentry = \"panel.luau\"\nheight = \"content\"\n",
       R"(panel entry 'panel': height = "content" requires plugin_api >= 42)"},
      {"plugin_api = 42\n[[panel]]\nid = \"panel\"\nentry = \"panel.luau\"\nheight = \"content\"\n",
       R"(panel entry 'panel': height = "content" requires max_height)"},
      {"plugin_api = 42\n[[panel]]\nid = \"panel\"\nentry = \"panel.luau\"\nheight = 300\nmax_height = 500\n",
       R"(panel entry 'panel': max_height requires height = "content")"},
      {"plugin_api = 42\n[[panel]]\nid = \"panel\"\nentry = \"panel.luau\"\nheight = \"content\"\nmax_height = 0\n",
       "panel entry 'panel': max_height must be a positive number"},
  };
  for (std::size_t i = 0; i < contentErrors.size(); ++i) {
    const auto path = root / ("content-error-" + std::to_string(i)) / "plugin.toml";
    TEST_CHECK(writeManifest(path, "id = \"me/content-error\"\nname = \"Content Error\"\n" + contentErrors[i].first));
    error.clear();
    TEST_CHECK(!scripting::parsePluginManifest(path, &error).has_value());
    TEST_CHECK(error == contentErrors[i].second);
  }

  const auto negativeSizeManifestPath = root / "negative-size/plugin.toml";
  TEST_CHECK(writeManifest(
      negativeSizeManifestPath,
      "id = \"me/negative-size\"\n"
      "name = \"Negative Size\"\n"
      "plugin_api = 3\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
      "width = -5\n"
  ));
  error.clear();
  const auto negativeSize = scripting::parsePluginManifest(negativeSizeManifestPath, &error);
  TEST_CHECK(!negativeSize.has_value());
  TEST_CHECK(error == "panel entry 'panel': width must be a positive number or \"fill\"");

  const auto fillAttachedManifestPath = root / "fill-attached/plugin.toml";
  TEST_CHECK(writeManifest(
      fillAttachedManifestPath,
      "id = \"me/fill-attached\"\n"
      "name = \"Fill Attached\"\n"
      "plugin_api = 3\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
      "height = \"fill\"\n"
      "placement = \"attached\"\n"
  ));
  error.clear();
  const auto fillAttached = scripting::parsePluginManifest(fillAttachedManifestPath, &error);
  TEST_CHECK(!fillAttached.has_value());
  TEST_CHECK(error == R"(panel entry 'panel': width/height "fill" requires placement = "floating")");

  const auto missingNameManifestPath = root / "missing-name/plugin.toml";
  TEST_CHECK(writeManifest(missingNameManifestPath, "id = \"me/missing-name\"\nplugin_api = 3\n"));
  error.clear();
  const auto missingName = scripting::parsePluginManifest(missingNameManifestPath, &error);
  TEST_CHECK(!missingName.has_value());
  TEST_CHECK(error == "missing mandatory key 'name'");

  const auto missingPluginApiPath = root / "missing-plugin-api/plugin.toml";
  TEST_CHECK(writeManifest(missingPluginApiPath, "id = \"me/missing-api\"\nname = \"Missing API\"\n"));
  error.clear();
  const auto missingPluginApi = scripting::parsePluginManifest(missingPluginApiPath, &error);
  TEST_CHECK(!missingPluginApi.has_value());
  TEST_CHECK(error == "missing mandatory key 'plugin_api'");

  const auto invalidPluginApiPath = root / "invalid-plugin-api/plugin.toml";
  TEST_CHECK(
      writeManifest(invalidPluginApiPath, "id = \"me/invalid-api\"\nname = \"Invalid API\"\nplugin_api = \"3\"\n")
  );
  error.clear();
  const auto invalidPluginApi = scripting::parsePluginManifest(invalidPluginApiPath, &error);
  TEST_CHECK(!invalidPluginApi.has_value());
  TEST_CHECK(error == "invalid 'plugin_api' (expected a positive integer)");

  const auto zeroPluginApiPath = root / "zero-plugin-api/plugin.toml";
  TEST_CHECK(writeManifest(zeroPluginApiPath, "id = \"me/zero-api\"\nname = \"Zero API\"\nplugin_api = 0\n"));
  error.clear();
  const auto zeroPluginApi = scripting::parsePluginManifest(zeroPluginApiPath, &error);
  TEST_CHECK(!zeroPluginApi.has_value());
  TEST_CHECK(error == "invalid 'plugin_api' (expected a positive integer)");

  const auto oldApiDismissPath = root / "old-api-dismiss/plugin.toml";
  TEST_CHECK(writeManifest(
      oldApiDismissPath,
      "id = \"me/old-api-dismiss\"\n"
      "name = \"Old API Dismiss\"\n"
      "plugin_api = 7\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
      "dismiss_on_outside_click = false\n"
  ));
  error.clear();
  const auto oldApiDismiss = scripting::parsePluginManifest(oldApiDismissPath, &error);
  TEST_CHECK(!oldApiDismiss.has_value());
  TEST_CHECK(error == "panel entry 'panel': dismiss_on_outside_click requires plugin_api >= 8");

  const auto dismissPanelPath = root / "dismiss-panel/plugin.toml";
  TEST_CHECK(writeManifest(
      dismissPanelPath,
      "id = \"me/dismiss-panel\"\n"
      "name = \"Dismiss Panel\"\n"
      "plugin_api = 8\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
      "dismiss_on_outside_click = false\n"
  ));
  error.clear();
  const auto dismissPanel = scripting::parsePluginManifest(dismissPanelPath, &error);
  TEST_CHECK(dismissPanel.has_value());
  TEST_CHECK(dismissPanel->entries.size() == 1);
  TEST_CHECK(!dismissPanel->entries.front().panelDismissOnOutsideClick);

  const auto oldApiKeyboardPath = root / "old-api-keyboard/plugin.toml";
  TEST_CHECK(writeManifest(
      oldApiKeyboardPath,
      "id = \"me/old-api-keyboard\"\n"
      "name = \"Old API Keyboard\"\n"
      "plugin_api = 9\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
      "keyboard_focus = \"none\"\n"
  ));
  error.clear();
  const auto oldApiKeyboard = scripting::parsePluginManifest(oldApiKeyboardPath, &error);
  TEST_CHECK(!oldApiKeyboard.has_value());
  TEST_CHECK(error == "panel entry 'panel': keyboard_focus requires plugin_api >= 10");

  const auto badKeyboardPath = root / "bad-keyboard/plugin.toml";
  TEST_CHECK(writeManifest(
      badKeyboardPath,
      "id = \"me/bad-keyboard\"\n"
      "name = \"Bad Keyboard\"\n"
      "plugin_api = 10\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
      "keyboard_focus = \"sometimes\"\n"
  ));
  error.clear();
  const auto badKeyboard = scripting::parsePluginManifest(badKeyboardPath, &error);
  TEST_CHECK(!badKeyboard.has_value());
  TEST_CHECK(error == R"(panel entry 'panel': keyboard_focus must be "on_demand", "exclusive" or "none")");

  const auto keyboardDismissPath = root / "keyboard-dismiss/plugin.toml";
  TEST_CHECK(writeManifest(
      keyboardDismissPath,
      "id = \"me/keyboard-dismiss\"\n"
      "name = \"Keyboard Dismiss\"\n"
      "plugin_api = 10\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
      "keyboard_focus = \"none\"\n"
  ));
  error.clear();
  const auto keyboardDismiss = scripting::parsePluginManifest(keyboardDismissPath, &error);
  TEST_CHECK(!keyboardDismiss.has_value());
  TEST_CHECK(error == R"(panel entry 'panel': keyboard_focus = "none" requires dismiss_on_outside_click = false)");

  const auto keyboardPanelPath = root / "keyboard-panel/plugin.toml";
  TEST_CHECK(writeManifest(
      keyboardPanelPath,
      "id = \"me/keyboard-panel\"\n"
      "name = \"Keyboard Panel\"\n"
      "plugin_api = 10\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
      "keyboard_focus = \"none\"\n"
      "dismiss_on_outside_click = false\n"
  ));
  error.clear();
  const auto keyboardPanel = scripting::parsePluginManifest(keyboardPanelPath, &error);
  TEST_CHECK(keyboardPanel.has_value());
  TEST_CHECK(keyboardPanel->entries.size() == 1);
  TEST_CHECK(keyboardPanel->entries.front().panelKeyboardFocus == "none");

  const auto defaultKeyboardPath = root / "default-keyboard/plugin.toml";
  TEST_CHECK(writeManifest(
      defaultKeyboardPath,
      "id = \"me/default-keyboard\"\n"
      "name = \"Default Keyboard\"\n"
      "plugin_api = 10\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
  ));
  error.clear();
  const auto defaultKeyboard = scripting::parsePluginManifest(defaultKeyboardPath, &error);
  TEST_CHECK(defaultKeyboard.has_value());
  TEST_CHECK(defaultKeyboard->entries.size() == 1);
  TEST_CHECK(defaultKeyboard->entries.front().panelKeyboardFocus == "on_demand");

  const auto oldApiPersistentPath = root / "old-api-persistent/plugin.toml";
  TEST_CHECK(writeManifest(
      oldApiPersistentPath,
      "id = \"me/old-api-persistent\"\n"
      "name = \"Old API Persistent\"\n"
      "plugin_api = 10\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
      "persistent = true\n"
  ));
  error.clear();
  const auto oldApiPersistent = scripting::parsePluginManifest(oldApiPersistentPath, &error);
  TEST_CHECK(!oldApiPersistent.has_value());
  TEST_CHECK(error == "panel entry 'panel': persistent requires plugin_api >= 11");

  const auto persistentDismissPath = root / "persistent-dismiss/plugin.toml";
  TEST_CHECK(writeManifest(
      persistentDismissPath,
      "id = \"me/persistent-dismiss\"\n"
      "name = \"Persistent Dismiss\"\n"
      "plugin_api = 11\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
      "persistent = true\n"
  ));
  error.clear();
  const auto persistentDismiss = scripting::parsePluginManifest(persistentDismissPath, &error);
  TEST_CHECK(!persistentDismiss.has_value());
  TEST_CHECK(error == "panel entry 'panel': persistent = true requires dismiss_on_outside_click = false");

  const auto persistentExclusivePath = root / "persistent-exclusive/plugin.toml";
  TEST_CHECK(writeManifest(
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
  ));
  error.clear();
  const auto persistentExclusive = scripting::parsePluginManifest(persistentExclusivePath, &error);
  TEST_CHECK(!persistentExclusive.has_value());
  TEST_CHECK(error == R"(panel entry 'panel': persistent = true is incompatible with keyboard_focus = "exclusive")");

  const auto persistentAttachedPath = root / "persistent-attached/plugin.toml";
  TEST_CHECK(writeManifest(
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
  ));
  error.clear();
  const auto persistentAttached = scripting::parsePluginManifest(persistentAttachedPath, &error);
  TEST_CHECK(!persistentAttached.has_value());
  TEST_CHECK(error == R"(panel entry 'panel': persistent = true requires placement = "floating")");

  const auto oskPanelPath = root / "osk-panel/plugin.toml";
  TEST_CHECK(writeManifest(
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
  ));
  error.clear();
  const auto oskPanel = scripting::parsePluginManifest(oskPanelPath, &error);
  TEST_CHECK(oskPanel.has_value());
  TEST_CHECK(oskPanel->entries.size() == 1);
  {
    const auto& entry = oskPanel->entries.front();
    TEST_CHECK(entry.panelPersistent);
    TEST_CHECK(entry.panelKeyboardFocus == "none");
    TEST_CHECK(entry.panelWidthFill);
    // A persistent panel gets no placement / open_near_click settings: it is always
    // floating and never opened from a bar widget's anchor.
    const bool hasPlacement = std::ranges::any_of(entry.settings, [](const scripting::ManifestField& field) {
      return field.key == "panel_placement";
    });
    const bool hasPosition = std::ranges::any_of(entry.settings, [](const scripting::ManifestField& field) {
      return field.key == "panel_position";
    });
    TEST_CHECK(!hasPlacement);
    TEST_CHECK(hasPosition);
  }

  const auto oldApiCapturePath = root / "old-api-capture/plugin.toml";
  TEST_CHECK(writeManifest(
      oldApiCapturePath,
      "id = \"me/old-api-capture\"\n"
      "name = \"Old API Capture\"\n"
      "plugin_api = 12\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
      "capture_keys = [\"space\"]\n"
  ));
  error.clear();
  TEST_CHECK(!scripting::parsePluginManifest(oldApiCapturePath, &error).has_value());
  TEST_CHECK(error == "panel entry 'panel': capture_keys requires plugin_api >= 13");

  const auto badCapturePath = root / "bad-capture/plugin.toml";
  TEST_CHECK(writeManifest(
      badCapturePath,
      "id = \"me/bad-capture\"\n"
      "name = \"Bad Capture\"\n"
      "plugin_api = 13\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
      "capture_keys = [\"space\", \"nonsensekey\"]\n"
  ));
  error.clear();
  TEST_CHECK(!scripting::parsePluginManifest(badCapturePath, &error).has_value());
  TEST_CHECK(error == "panel entry 'panel': capture_keys entry 'nonsensekey' is not a valid key chord");

  // A Super chord belongs to the compositor; parseKeyChordSpec throws rather than returning
  // nullopt for it, so this covers the other rejection path.
  const auto superCapturePath = root / "super-capture/plugin.toml";
  TEST_CHECK(writeManifest(
      superCapturePath,
      "id = \"me/super-capture\"\n"
      "name = \"Super Capture\"\n"
      "plugin_api = 13\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
      "capture_keys = [\"super+space\"]\n"
  ));
  error.clear();
  TEST_CHECK(!scripting::parsePluginManifest(superCapturePath, &error).has_value());
  TEST_CHECK(error.starts_with("panel entry 'panel': capture_keys entry 'super+space': "));

  const auto captureNoFocusPath = root / "capture-no-focus/plugin.toml";
  TEST_CHECK(writeManifest(
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
  ));
  error.clear();
  TEST_CHECK(!scripting::parsePluginManifest(captureNoFocusPath, &error).has_value());
  TEST_CHECK(error == R"(panel entry 'panel': capture_keys requires keyboard_focus "on_demand" or "exclusive")");

  const auto capturePath = root / "capture/plugin.toml";
  TEST_CHECK(writeManifest(
      capturePath,
      "id = \"me/capture\"\n"
      "name = \"Capture\"\n"
      "plugin_api = 13\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
      "capture_keys = [\"space\", \"ctrl+r\"]\n"
  ));
  error.clear();
  const auto capture = scripting::parsePluginManifest(capturePath, &error);
  TEST_CHECK(capture.has_value());
  if (!capture->entries.empty()) {
    const auto& keys = capture->entries.front().panelCaptureKeys;
    TEST_CHECK(keys.size() == 2);
    // Stored verbatim: the script is called back with the exact spec it declared.
    TEST_CHECK(keys[0] == "space");
    TEST_CHECK(keys[1] == "ctrl+r");
  }

  const auto noCapturePath = root / "no-capture/plugin.toml";
  TEST_CHECK(writeManifest(
      noCapturePath,
      "id = \"me/no-capture\"\n"
      "name = \"No Capture\"\n"
      "plugin_api = 13\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
  ));
  error.clear();
  const auto noCapture = scripting::parsePluginManifest(noCapturePath, &error);
  TEST_CHECK(noCapture.has_value());
  if (!noCapture->entries.empty()) {
    const auto& entry = noCapture->entries.front();
    TEST_CHECK(entry.panelCaptureKeys.empty());
    TEST_CHECK(entry.panelLayerDefault == "follow");
    const auto layerField = std::ranges::find(entry.settings, "panel_layer", &scripting::ManifestField::key);
    TEST_CHECK(layerField != entry.settings.end());
    TEST_CHECK(layerField->stringDefault == "follow");
    const auto seededDefaults = scripting::seedEntrySettings(entry, {});
    const auto defaultShellConfig = scripting::resolvePluginPanelShellConfig(entry, seededDefaults);
    TEST_CHECK(defaultShellConfig.layer == "follow");
    TEST_CHECK(scripting::panelLayerFollowsFloating(defaultShellConfig.layer));
    const auto unknownSettings = scripting::seedEntrySettings(entry, {{"panel_layer", std::string("bottom")}});
    const auto unknownShellConfig = scripting::resolvePluginPanelShellConfig(entry, unknownSettings);
    TEST_CHECK(unknownShellConfig.layer == "follow");
  }

  const auto oldApiLayerPath = root / "old-api-layer/plugin.toml";
  TEST_CHECK(writeManifest(
      oldApiLayerPath,
      "id = \"me/old-api-layer\"\n"
      "name = \"Old API Layer\"\n"
      "plugin_api = 29\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
      "layer = \"overlay\"\n"
  ));
  error.clear();
  TEST_CHECK(!scripting::parsePluginManifest(oldApiLayerPath, &error).has_value());
  TEST_CHECK(error == "panel entry 'panel': layer requires plugin_api >= 30");

  const auto badLayerPath = root / "bad-layer/plugin.toml";
  TEST_CHECK(writeManifest(
      badLayerPath,
      "id = \"me/bad-layer\"\n"
      "name = \"Bad Layer\"\n"
      "plugin_api = 30\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
      "layer = \"bottom\"\n"
  ));
  error.clear();
  TEST_CHECK(!scripting::parsePluginManifest(badLayerPath, &error).has_value());
  TEST_CHECK(error == R"(panel entry 'panel': layer must be "follow", "top" or "overlay")");

  const auto overlayLayerPath = root / "overlay-layer/plugin.toml";
  TEST_CHECK(writeManifest(
      overlayLayerPath,
      "id = \"me/overlay-layer\"\n"
      "name = \"Overlay Layer\"\n"
      "plugin_api = 30\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
      "layer = \"overlay\"\n"
  ));
  error.clear();
  const auto overlayLayer = scripting::parsePluginManifest(overlayLayerPath, &error);
  TEST_CHECK(overlayLayer.has_value());
  if (!overlayLayer->entries.empty()) {
    const auto& entry = overlayLayer->entries.front();
    TEST_CHECK(entry.panelLayerDefault == "overlay");
    const auto layerField = std::ranges::find(entry.settings, "panel_layer", &scripting::ManifestField::key);
    TEST_CHECK(layerField != entry.settings.end());
    TEST_CHECK(layerField->stringDefault == "overlay");
    const auto settings = scripting::seedEntrySettings(entry, {{"panel_layer", std::string("top")}});
    const auto shellConfig = scripting::resolvePluginPanelShellConfig(entry, settings);
    TEST_CHECK(shellConfig.layer == "top");
    TEST_CHECK(!scripting::panelLayerFollowsFloating(shellConfig.layer));
  }

  const auto oldApiFollowLayerPath = root / "old-api-follow-layer/plugin.toml";
  TEST_CHECK(writeManifest(
      oldApiFollowLayerPath,
      "id = \"me/old-api-follow-layer\"\n"
      "name = \"Old API Follow Layer\"\n"
      "plugin_api = 48\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
      "layer = \"follow\"\n"
  ));
  error.clear();
  TEST_CHECK(!scripting::parsePluginManifest(oldApiFollowLayerPath, &error).has_value());
  TEST_CHECK(error == R"(panel entry 'panel': layer "follow" requires plugin_api >= 49)");

  const auto followLayerPath = root / "follow-layer/plugin.toml";
  TEST_CHECK(writeManifest(
      followLayerPath,
      "id = \"me/follow-layer\"\n"
      "name = \"Follow Layer\"\n"
      "plugin_api = 49\n"
      "[[panel]]\n"
      "id = \"panel\"\n"
      "entry = \"panel.luau\"\n"
      "layer = \"follow\"\n"
  ));
  error.clear();
  const auto followLayer = scripting::parsePluginManifest(followLayerPath, &error);
  TEST_CHECK(followLayer.has_value());
  if (!followLayer->entries.empty()) {
    const auto& entry = followLayer->entries.front();
    TEST_CHECK(entry.panelLayerDefault == "follow");
    const auto settings = scripting::seedEntrySettings(entry, {});
    const auto shellConfig = scripting::resolvePluginPanelShellConfig(entry, settings);
    TEST_CHECK(shellConfig.layer == "follow");
  }
  TEST_CHECK(scripting::isValidPanelLayer("follow"));
  TEST_CHECK(scripting::isValidPanelLayer("top"));
  TEST_CHECK(scripting::isValidPanelLayer("overlay"));
  TEST_CHECK(!scripting::isValidPanelLayer("bottom"));

  // A [[widget]] entry can declare bar gesture defaults, kept as raw strings: the gesture
  // vocabulary and the action grammar belong to the bar, not to the manifest parser.
  const auto actionsPath = root / "actions" / "plugin.toml";
  TEST_CHECK(
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
      )
      && !actionsPath.empty()
  );
  error.clear();
  const auto actions = scripting::parsePluginManifest(actionsPath, &error);
  TEST_CHECK(actions.has_value());
  if (!actions->entries.empty()) {
    const auto& declared = actions->entries.front().widgetActions;
    TEST_CHECK(declared.size() == 2);
    const auto middle = std::ranges::find(declared, "middle", &std::pair<std::string, std::string>::first);
    TEST_CHECK(middle != declared.end());
    TEST_CHECK(middle->second == "exec playerctl pause");
  }

  // The capability is gated on its API level, like every other manifest addition.
  const auto oldApiPath = root / "old-api" / "plugin.toml";
  TEST_CHECK(writeManifest(
      oldApiPath,
      "id = \"me/old-api\"\n"
      "name = \"Old Api\"\n"
      "plugin_api = 13\n"
      "[[widget]]\n"
      "id = \"bar\"\n"
      "entry = \"bar.luau\"\n"
      "[widget.actions]\n"
      "middle = \"volume-mute\"\n"
  ));
  error.clear();
  TEST_CHECK(!scripting::parsePluginManifest(oldApiPath, &error).has_value());

  // A non-string action is a manifest error rather than something to guess at.
  const auto badActionPath = root / "bad-action" / "plugin.toml";
  TEST_CHECK(writeManifest(
      badActionPath,
      "id = \"me/bad-action\"\n"
      "name = \"Bad Action\"\n"
      "plugin_api = 14\n"
      "[[widget]]\n"
      "id = \"bar\"\n"
      "entry = \"bar.luau\"\n"
      "[widget.actions]\n"
      "middle = 42\n"
  ));
  error.clear();
  TEST_CHECK(!scripting::parsePluginManifest(badActionPath, &error).has_value());

  // A [[widget]] entry can opt out of the bar's whole-widget hover highlight. The key sits on
  // the entry itself, ahead of any [widget.*] subtable.
  const auto hoverPath = root / "hover-highlight" / "plugin.toml";
  TEST_CHECK(writeManifest(
      hoverPath,
      "id = \"me/hover-highlight\"\n"
      "name = \"Hover Highlight\"\n"
      "plugin_api = 34\n"
      "[[widget]]\n"
      "id = \"bar\"\n"
      "entry = \"bar.luau\"\n"
      "hover_highlight = false\n"
  ));
  error.clear();
  const auto hover = scripting::parsePluginManifest(hoverPath, &error);
  TEST_CHECK(hover.has_value());
  if (!hover->entries.empty()) {
    TEST_CHECK(!hover->entries.front().widgetHoverHighlight);
  }

  const auto hoverOldApiPath = root / "hover-old-api" / "plugin.toml";
  TEST_CHECK(writeManifest(
      hoverOldApiPath,
      "id = \"me/hover-old-api\"\n"
      "name = \"Hover Old Api\"\n"
      "plugin_api = 33\n"
      "[[widget]]\n"
      "id = \"bar\"\n"
      "entry = \"bar.luau\"\n"
      "hover_highlight = false\n"
  ));
  error.clear();
  TEST_CHECK(!scripting::parsePluginManifest(hoverOldApiPath, &error).has_value());
  TEST_CHECK(error == "widget entry 'bar': hover_highlight requires plugin_api >= 34");

  const auto hoverBadPath = root / "hover-bad" / "plugin.toml";
  TEST_CHECK(writeManifest(
      hoverBadPath,
      "id = \"me/hover-bad\"\n"
      "name = \"Hover Bad\"\n"
      "plugin_api = 34\n"
      "[[widget]]\n"
      "id = \"bar\"\n"
      "entry = \"bar.luau\"\n"
      "hover_highlight = \"no\"\n"
  ));
  error.clear();
  TEST_CHECK(!scripting::parsePluginManifest(hoverBadPath, &error).has_value());
  TEST_CHECK(error == "widget entry 'bar': hover_highlight must be a bool");

  // A [[layer]] entry carries its surface defaults; every declared key lands in layerConfig.
  const auto layerPath = root / "layer" / "plugin.toml";
  TEST_CHECK(writeManifest(
      layerPath,
      "id = \"me/layer\"\n"
      "name = \"Layer\"\n"
      "plugin_api = 36\n"
      "[[layer]]\n"
      "id = \"bar\"\n"
      "entry = \"bar.luau\"\n"
      "layer = \"bottom\"\n"
      "anchors = [\"top\", \"left\", \"right\"]\n"
      "height = 32\n"
      "margins = { top = 4, left = -2 }\n"
      "exclusive_zone = \"auto\"\n"
      "keyboard = \"on_demand\"\n"
      "output = \"DP-1\"\n"
      "visible = true\n"
  ));
  error.clear();
  const auto layer = scripting::parsePluginManifest(layerPath, &error);
  TEST_CHECK(layer.has_value());
  if (!layer->entries.empty()) {
    const auto& entry = layer->entries.front();
    const auto& config = entry.layerConfig;
    TEST_CHECK(entry.kind == scripting::PluginEntryKind::Layer);
    TEST_CHECK(config.layer == scripting::PluginLayerLevel::Bottom);
    TEST_CHECK(
        config.anchors
        == (scripting::PluginLayerAnchor::Top
            | scripting::PluginLayerAnchor::Left
            | scripting::PluginLayerAnchor::Right)
    );
    TEST_CHECK(config.width == 0 && config.height == 32);
    TEST_CHECK(config.marginTop == 4 && config.marginLeft == -2 && config.marginRight == 0 && config.marginBottom == 0);
    TEST_CHECK(config.exclusiveZoneAuto);
    TEST_CHECK(config.keyboard == scripting::PluginLayerKeyboard::OnDemand);
    TEST_CHECK(config.output == "DP-1");
    TEST_CHECK(entry.layerVisible);
  }

  const auto layerOldApiPath = root / "layer-old-api" / "plugin.toml";
  TEST_CHECK(writeManifest(
      layerOldApiPath,
      "id = \"me/layer-old-api\"\n"
      "name = \"Layer Old Api\"\n"
      "plugin_api = 35\n"
      "[[layer]]\n"
      "id = \"bar\"\n"
      "entry = \"bar.luau\"\n"
  ));
  error.clear();
  TEST_CHECK(!scripting::parsePluginManifest(layerOldApiPath, &error).has_value());
  TEST_CHECK(error == "layer entry 'bar' requires plugin_api >= 36");

  const std::pair<std::string_view, std::string_view> badLayers[] = {
      {"layer = \"desktop\"", R"(layer entry 'bar': layer must be "background", "bottom", "top", or "overlay")"},
      {R"(anchors = ["top", "top"])",
       R"(layer entry 'bar': anchors must list distinct values from "top", "bottom", "left", "right")"},
      {"width = -1", "layer entry 'bar': width must be an integer between 0 and 65536"},
      {"margins = { middle = 3 }", "layer entry 'bar': margins must be a table with integer top, right, bottom, left"},
      {"exclusive_zone = -2", R"(layer entry 'bar': exclusive_zone must be "auto" or an integer >= -1)"},
      {"keyboard = \"grab\"", R"(layer entry 'bar': keyboard must be "none", "on_demand", or "exclusive")"},
      {"output = \"\"", R"(layer entry 'bar': output must be "all", "focused", or an output name)"},
      {"visible = \"yes\"", "layer entry 'bar': visible must be a bool"},
  };
  for (const auto& [line, expected] : badLayers) {
    const auto badPath = root / "layer-bad" / "plugin.toml";
    TEST_CHECK(writeManifest(
        badPath,
        "id = \"me/layer-bad\"\nname = \"Layer Bad\"\nplugin_api = 36\n[[layer]]\nid = \"bar\"\n"
        "entry = \"bar.luau\"\n"
            + std::string(line)
            + "\n"
    ));
    error.clear();
    TEST_CHECK(!scripting::parsePluginManifest(badPath, &error).has_value());
    TEST_CHECK(error == expected);
  }

  return 0;
}
