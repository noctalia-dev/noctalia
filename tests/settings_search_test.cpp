// Every setting must be found by its own translated title, typed in lower or upper case,
// in every supported language (issue #4982: case-insensitive search only handled ASCII).

#include "config/config_types.h"
#include "i18n/i18n_service.h"
#include "shell/settings/settings_registry.h"
#include "shell/settings/supported_languages.h"
#include "tests/test_check.h"

#include <cstdio>
#include <cstdlib>
#include <glib.h>
#include <print>
#include <string>
#include <string_view>

namespace {

  std::string changeCase(std::string_view text, bool upper) {
    const auto length = static_cast<gssize>(text.size());
    gchar* changed = upper ? g_utf8_strup(text.data(), length) : g_utf8_strdown(text.data(), length);
    std::string out(changed);
    g_free(changed);
    return out;
  }

} // namespace

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::println(stderr, "settings_search_test: expected assets directory");
    return 1;
  }
  setenv("NOCTALIA_ASSETS_DIR", argv[1], 1);

  const Config config{};
  int totalFailures = 0;
  for (const auto& language : settings::kSupportedLanguages) {
    i18n::Service::instance().init(language.code);
    TEST_CHECK(i18n::Service::instance().language() == language.code);
    const auto registry = settings::buildSettingsRegistry(config, nullptr);
    TEST_CHECK(!registry.empty());
    int failures = 0;
    for (const auto& entry : registry) {
      if (entry.title.empty()) {
        continue;
      }
      for (const bool upper : {false, true}) {
        const std::string query = changeCase(entry.title, upper);
        if (!settings::matchesSettingQuery(entry, query) && ++failures <= 3) {
          std::println(stderr, "settings_search_test: {}: '{}' not found by '{}'", language.code, entry.title, query);
        }
      }
    }
    if (failures > 3) {
      std::println(stderr, "settings_search_test: {}: {} failures in total", language.code, failures);
    }
    totalFailures += failures;
  }
  TEST_CHECK(totalFailures == 0);
}
