#include "i18n/i18n_service.h"
#include "scripting/plugin_api.h"
#include "scripting/plugin_i18n.h"
#include "scripting/plugin_registry.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

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
  TEST_CHECK(!root.empty());

  const auto sourceRoot = root / "sources";
  const auto pluginRoot = sourceRoot / "example";
  const bool enWritten = writeText(
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
  );
  const bool frWritten = writeText(
      pluginRoot / "translations/fr.json",
      "{\n"
      "  \"settings\": {\n"
      "    \"mode\": { \"label\": \"Mode FR\" },\n"
      "    \"shared\": \"Français\"\n"
      "  }\n"
      "}\n"
  );
  const bool manifestWritten = writeText(
      pluginRoot / "plugin.toml",
      "id = \"test/example\"\n"
      "name = \"Example\"\n"
      "version = \"1.0.0\"\n"
      "plugin_api = "
          + std::to_string(scripting::kCurrentPluginApiVersion)
          + "\n"
            "[[widget]]\n"
            "id = \"hello\"\n"
            "entry = \"src/hello.luau\"\n"
  );
  const bool entryWritten = writeText(pluginRoot / "src/hello.luau", "function onUpdate() end\n");

  i18n::Service::instance().init("fr");

  auto& registry = scripting::PluginRegistry::instance();
  registry.setSources({sourceRoot});
  registry.scan();
  const auto resolved = registry.resolve("test/example:hello");
  scripting::PluginTranslationCatalog catalog;
  if (resolved.has_value()) {
    catalog.load(resolved->pluginDir);
  }

  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  TEST_CHECK(enWritten);
  TEST_CHECK(frWritten);
  TEST_CHECK(manifestWritten);
  TEST_CHECK(entryWritten);
  TEST_CHECK(resolved.has_value());
  TEST_CHECK(resolved->sourcePath == pluginRoot / "src/hello.luau");
  TEST_CHECK(resolved->pluginDir == pluginRoot);
  // nested entry should translate from the plugin root
  TEST_CHECK(catalog.translate("title") == "Example");
  TEST_CHECK(catalog.has("settings.mode.label"));
  // active language should override English
  TEST_CHECK(catalog.translate("settings.mode.label") == "Mode FR");
  // English fallback should interpolate
  TEST_CHECK(catalog.translate("settings.mode.description", {{"name", "display"}}) == "Choose display mode");
  TEST_CHECK(catalog.translate("settings.missing") == "settings.missing");
  return 0;
}
