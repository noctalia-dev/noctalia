#include "shell/settings/supported_languages.h"
#include "tests/test_check.h"

#include <cstdio>
#include <filesystem>
#include <print>
#include <set>
#include <string>

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::println(stderr, "i18n_supported_languages_test: expected translations directory");
    return 1;
  }

  const std::filesystem::path translationsDir(argv[1]);

  std::set<std::string> supported;
  for (const auto& language : settings::kSupportedLanguages) {
    supported.insert(std::string(language.code));
  }

  for (const auto& language : settings::kSupportedLanguages) {
    const std::filesystem::path path = translationsDir / (std::string(language.code) + ".json");
    const bool hasCatalog = std::filesystem::is_regular_file(path);
    if (!hasCatalog) {
      std::println(stderr, "i18n_supported_languages_test: supported language has no catalog: {}", language.code);
    }
    TEST_CHECK(hasCatalog);
  }

  for (const auto& entry : std::filesystem::directory_iterator(translationsDir)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json") {
      continue;
    }
    const std::string code = entry.path().stem().string();
    const bool isSupported = supported.contains(code);
    if (!isSupported) {
      std::println(stderr, "i18n_supported_languages_test: catalog is missing from kSupportedLanguages: {}", code);
    }
    TEST_CHECK(isSupported);
  }

  return 0;
}
