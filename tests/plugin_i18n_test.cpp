#include "i18n/i18n_service.h"
#include "scripting/plugin_api.h"
#include "scripting/plugin_i18n.h"
#include "scripting/plugin_registry.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "plugin_i18n_test: {}", message);
    }
    return condition;
  }

  bool expectEq(std::string_view actual, std::string_view expected, const char* message) {
    if (actual != expected) {
      std::println(stderr, "plugin_i18n_test: {}\n  actual:   {}\n  expected: {}", message, actual, expected);
      return false;
    }
    return true;
  }

  std::filesystem::path makeTempDir() {
    std::string pattern = (std::filesystem::temp_directory_path() / "noctalia-plugin-i18n-XXXXXX").string();
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

  const auto sourceRoot = root / "sources";
  const auto pluginRoot = sourceRoot / "example";
  bool ok = true;
  ok = writeText(
           pluginRoot / "translations/en.json",
           "{\n"
           "  \"title\": \"Example\",\n"
           "  \"settings\": {\n"
           "    \"mode\": {\n"
           "      \"label\": \"Mode\",\n"
           "      \"description\": \"Choose {name} mode\"\n"
           "    },\n"
           "    \"shared\": \"English\"\n"
           "  }\n"
           "}\n"
       )
      && ok;
  ok = writeText(
           pluginRoot / "translations/fr.json",
           "{\n"
           "  \"settings\": {\n"
           "    \"mode\": { \"label\": \"Mode FR\" },\n"
           "    \"shared\": \"Français\"\n"
           "  }\n"
           "}\n"
       )
      && ok;
  ok = writeText(
           pluginRoot / "plugin.toml",
           "id = \"test/example\"\n"
           "name = \"Example\"\n"
           "plugin_api = "
               + std::to_string(scripting::kCurrentPluginApiVersion)
               + "\n"
                 "[[widget]]\n"
                 "id = \"hello\"\n"
                 "entry = \"src/hello.luau\"\n"
       )
      && ok;
  ok = writeText(pluginRoot / "src/hello.luau", "function onUpdate() end\n") && ok;

  i18n::Service::instance().init("fr");

  auto& registry = scripting::PluginRegistry::instance();
  registry.setSources({sourceRoot});
  registry.scan();
  const auto resolved = registry.resolve("test/example:hello");
  if (!expect(resolved.has_value(), "nested plugin entry should resolve")) {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    return 1;
  }

  ok = expect(resolved->sourcePath == pluginRoot / "src/hello.luau", "nested entry source path should be preserved")
      && ok;
  ok = expect(resolved->pluginDir == pluginRoot, "resolved entry should retain the plugin root") && ok;

  scripting::PluginTranslationCatalog catalog;
  catalog.load(resolved->pluginDir);
  ok = expectEq(catalog.translate("title"), "Example", "nested entry should translate from the plugin root") && ok;

  ok = expect(catalog.has("settings.mode.label"), "translated label key should exist") && ok;
  ok = expectEq(catalog.translate("settings.mode.label"), "Mode FR", "active language should override English") && ok;
  ok = expectEq(
           catalog.translate("settings.mode.description", {{"name", "display"}}), "Choose display mode",
           "English fallback should interpolate"
       )
      && ok;
  ok = expectEq(catalog.translate("settings.missing"), "settings.missing", "missing key should return key") && ok;

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  return ok ? 0 : 1;
}
