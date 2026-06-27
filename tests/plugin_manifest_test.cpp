#include "scripting/plugin_manifest.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::fprintf(stderr, "plugin_manifest_test: %s\n", message);
    }
    return condition;
  }

  bool expectEq(std::string_view actual, std::string_view expected, const char* message) {
    if (actual != expected) {
      std::fprintf(
          stderr, "plugin_manifest_test: %s\n  actual:   %.*s\n  expected: %.*s\n", message,
          static_cast<int>(actual.size()), actual.data(), static_cast<int>(expected.size()), expected.data()
      );
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
  const auto defaultManifestPath = root / "defaults/plugin.toml";
  ok = writeText(defaultManifestPath, "id = \"me/defaults\"\nname = \"Defaults\"\nmin_noctalia = \"5.0.0\"\n") && ok;

  std::string error;
  const auto defaults = scripting::parsePluginManifest(defaultManifestPath, &error);
  ok = expect(defaults.has_value(), error.empty() ? "failed to parse default manifest" : error.c_str()) && ok;
  if (defaults.has_value()) {
    ok = expectEq(defaults->license, "MIT", "license should default to MIT") && ok;
    ok = expect(!defaults->deprecated, "deprecated should default to false") && ok;
    ok = expect(defaults->dependencies.empty(), "dependencies should default to empty") && ok;
  }

  const auto explicitManifestPath = root / "explicit/plugin.toml";
  ok = writeText(
           explicitManifestPath,
           "id = \"me/explicit\"\n"
           "name = \"Explicit\"\n"
           "min_noctalia = \"5.0.0\"\n"
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
           "min_noctalia = \"5.0.0\"\n"
           "[[setting]]\n"
           "key = \"mode\"\n"
           "type = \"select\"\n"
           "label_key = \"settings.mode.label\"\n"
           "description_key = \"settings.mode.description\"\n"
           "default = \"auto\"\n"
           "options = [\n"
           "  { value = \"auto\", label_key = \"settings.mode.options.auto\" },\n"
           "  { value = \"manual\", label = \"Manual\" },\n"
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
        ok = expectEq(setting.options[1].label, "Manual", "literal select option label should parse") && ok;
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

  const auto labelConflictPath = root / "label-conflict/plugin.toml";
  ok = writeText(
           labelConflictPath,
           "id = \"me/label-conflict\"\n"
           "name = \"Label Conflict\"\n"
           "min_noctalia = \"5.0.0\"\n"
           "[[setting]]\n"
           "key = \"mode\"\n"
           "label = \"Mode\"\n"
           "label_key = \"settings.mode.label\"\n"
       )
      && ok;
  error.clear();
  const auto labelConflict = scripting::parsePluginManifest(labelConflictPath, &error);
  ok = expect(!labelConflict.has_value(), "label plus label_key should fail") && ok;
  ok = expectEq(error, "setting 'mode' declares both label and label_key", "label conflict error") && ok;

  const auto descriptionConflictPath = root / "description-conflict/plugin.toml";
  ok = writeText(
           descriptionConflictPath,
           "id = \"me/description-conflict\"\n"
           "name = \"Description Conflict\"\n"
           "min_noctalia = \"5.0.0\"\n"
           "[[setting]]\n"
           "key = \"mode\"\n"
           "description = \"Mode\"\n"
           "description_key = \"settings.mode.description\"\n"
       )
      && ok;
  error.clear();
  const auto descriptionConflict = scripting::parsePluginManifest(descriptionConflictPath, &error);
  ok = expect(!descriptionConflict.has_value(), "description plus description_key should fail") && ok;
  ok = expectEq(error, "setting 'mode' declares both description and description_key", "description conflict error")
      && ok;

  const auto optionConflictPath = root / "option-conflict/plugin.toml";
  ok = writeText(
           optionConflictPath,
           "id = \"me/option-conflict\"\n"
           "name = \"Option Conflict\"\n"
           "min_noctalia = \"5.0.0\"\n"
           "[[setting]]\n"
           "key = \"mode\"\n"
           "type = \"select\"\n"
           "default = \"auto\"\n"
           "options = [{ value = \"auto\", label = \"Auto\", label_key = \"settings.mode.options.auto\" }]\n"
       )
      && ok;
  error.clear();
  const auto optionConflict = scripting::parsePluginManifest(optionConflictPath, &error);
  ok = expect(!optionConflict.has_value(), "option label plus label_key should fail") && ok;
  ok = expectEq(error, "setting 'mode' option 'auto' declares both label and label_key", "option label conflict error")
      && ok;

  const auto launcherManifestPath = root / "launcher/plugin.toml";
  ok = writeText(
           launcherManifestPath,
           "id = \"me/launcher\"\n"
           "name = \"Launcher\"\n"
           "min_noctalia = \"5.0.0\"\n"
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

  const auto listManifestPath = root / "string-list/plugin.toml";
  ok = writeText(
           listManifestPath,
           "id = \"me/string-list\"\n"
           "name = \"String List\"\n"
           "min_noctalia = \"5.0.0\"\n"
           "[[widget]]\n"
           "id = \"list\"\n"
           "entry = \"list.luau\"\n"
           "[[widget.setting]]\n"
           "key = \"paths\"\n"
           "type = \"string_list\"\n"
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

  const auto missingNameManifestPath = root / "missing-name/plugin.toml";
  ok = writeText(missingNameManifestPath, "id = \"me/missing-name\"\nmin_noctalia = \"5.0.0\"\n") && ok;
  error.clear();
  const auto missingName = scripting::parsePluginManifest(missingNameManifestPath, &error);
  ok = expect(!missingName.has_value(), "manifest without name should fail") && ok;
  ok = expectEq(error, "missing mandatory key 'name'", "missing name error") && ok;

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  return ok ? 0 : 1;
}
