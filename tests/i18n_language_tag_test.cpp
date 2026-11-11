#include "i18n/language_tag.h"
#include "tests/test_check.h"

#include <string>
#include <vector>

int main() {
  TEST_CHECK(i18n::detail::normalizeLanguageTag("zh_CN.UTF-8") == "zh-CN");
  TEST_CHECK(i18n::detail::normalizeLanguageTag("pt-br") == "pt-BR");
  TEST_CHECK(i18n::detail::normalizeLanguageTag("zh_hans_cn") == "zh-Hans-CN");
  TEST_CHECK(i18n::detail::normalizeLanguageTag("C.UTF-8").empty());
  TEST_CHECK(i18n::detail::isRtlLanguage("ar"));
  TEST_CHECK(i18n::detail::isRtlLanguage("AR-EG"));
  // Kurmanji stays LTR
  TEST_CHECK(!i18n::detail::isRtlLanguage("ku"));
  TEST_CHECK(!i18n::detail::isRtlLanguage("en"));

  TEST_CHECK(
      i18n::detail::catalogLanguageCandidates("zh_CN.UTF-8")
      == std::vector<std::string>{"zh-Hans-CN", "zh-CN", "zh-Hans", "zh"}
  );
  TEST_CHECK(
      i18n::detail::catalogLanguageCandidates("zh_TW")
      == std::vector<std::string>{"zh-Hant-TW", "zh-TW", "zh-Hant", "zh"}
  );
  TEST_CHECK(
      i18n::detail::catalogLanguageCandidates("zh-Hans-CN")
      == std::vector<std::string>{"zh-Hans-CN", "zh-CN", "zh-Hans", "zh"}
  );
  TEST_CHECK(i18n::detail::catalogLanguageCandidates("pt_BR.UTF-8") == std::vector<std::string>{"pt-BR", "pt"});
  TEST_CHECK(i18n::detail::catalogLanguageCandidates("en_US") == std::vector<std::string>{"en-US", "en"});
  TEST_CHECK(i18n::detail::catalogLanguageCandidates("C.UTF-8").empty());

  return 0;
}
