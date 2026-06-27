#include "i18n/i18n_service.h"

#include <cstdio>
#include <filesystem>
#include <set>
#include <string>

namespace {

  bool fail(const char* message, const std::string& value) {
    std::fprintf(stderr, "i18n_supported_languages_test: %s: %s\n", message, value.c_str());
    return false;
  }

} // namespace

int main() {
  const std::filesystem::path translationsDir = std::filesystem::path(NOCTALIA_SOURCE_ASSETS_DIR) / "translations";

  std::set<std::string> supported;
  for (const auto& language : i18n::kSupportedLanguages) {
    supported.insert(std::string(language.code));
  }

  bool ok = true;
  for (const auto& language : i18n::kSupportedLanguages) {
    const std::filesystem::path path = translationsDir / (std::string(language.code) + ".json");
    if (!std::filesystem::is_regular_file(path)) {
      ok = fail("supported language has no catalog", std::string(language.code)) && ok;
    }
  }

  for (const auto& entry : std::filesystem::directory_iterator(translationsDir)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json") {
      continue;
    }
    const std::string code = entry.path().stem().string();
    if (!supported.contains(code)) {
      ok = fail("catalog is missing from kSupportedLanguages", code) && ok;
    }
  }

  return ok ? 0 : 1;
}
