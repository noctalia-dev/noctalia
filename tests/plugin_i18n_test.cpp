#include "i18n/i18n_service.h"
#include "scripting/plugin_i18n.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::fprintf(stderr, "plugin_i18n_test: %s\n", message);
    }
    return condition;
  }

  bool expectEq(std::string_view actual, std::string_view expected, const char* message) {
    if (actual != expected) {
      std::fprintf(
          stderr, "plugin_i18n_test: %s\n  actual:   %.*s\n  expected: %.*s\n", message,
          static_cast<int>(actual.size()), actual.data(), static_cast<int>(expected.size()), expected.data()
      );
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

  bool ok = true;
  ok = writeText(
           root / "translations/en.json",
           "{\n"
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
           root / "translations/fr.json",
           "{\n"
           "  \"settings\": {\n"
           "    \"mode\": { \"label\": \"Mode FR\" },\n"
           "    \"shared\": \"Français\"\n"
           "  }\n"
           "}\n"
       )
      && ok;

  i18n::Service::instance().init("fr");

  scripting::PluginTranslationCatalog catalog;
  catalog.load(root);

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
