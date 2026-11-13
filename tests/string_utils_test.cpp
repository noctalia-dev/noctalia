#include "tests/test_check.h"
#include "util/string_utils.h"

#include <string>
#include <vector>

using Words = std::vector<std::string>;

int main() {
  TEST_CHECK(StringUtils::urlEncode("abc-_.~09AZaz") == "abc-_.~09AZaz");
  TEST_CHECK(StringUtils::urlEncode("hello world") == "hello%20world");
  TEST_CHECK(StringUtils::urlEncode("dir/file?x=1&y=%") == "dir%2Ffile%3Fx%3D1%26y%3D%25");
  TEST_CHECK(StringUtils::urlEncode("\xC3\xA9") == "%C3%A9");

  TEST_CHECK(StringUtils::naturalCaseInsensitiveCompare("image(2)", "image(10)") < 0);
  TEST_CHECK(StringUtils::naturalCaseInsensitiveCompare("image(10)", "image(2)") > 0);
  TEST_CHECK(StringUtils::naturalCaseInsensitiveCompare("Image(1)", "image(1)") == 0);
  TEST_CHECK(StringUtils::naturalCaseInsensitiveCompare("a2", "a10") < 0);
  TEST_CHECK(StringUtils::naturalCaseInsensitiveCompare("file", "file1") < 0);

  TEST_CHECK(StringUtils::sanitizeMarkup("<b>bold</b><br>next") == "bold\nnext");
  // preserves angle-bracket text that is not notification markup
  TEST_CHECK(StringUtils::sanitizeMarkup("<.< wawd :>") == "<.< wawd :>");
  TEST_CHECK(StringUtils::sanitizeMarkup("<span>literal</span>") == "<span>literal</span>");
  TEST_CHECK(StringUtils::sanitizeMarkup("&lt;literal&gt;") == "<literal>");

  TEST_CHECK(StringUtils::normalizeForSearch("Bar Clock") == "bar clock");
  TEST_CHECK(StringUtils::normalizeForSearch("ШРИФТ Панели") == "шрифт панели");
  TEST_CHECK(StringUtils::normalizeForSearch("ÜBER Ära") == "uber ara");
  TEST_CHECK(StringUtils::normalizeForSearch("Épaisseur") == "epaisseur");
  TEST_CHECK(StringUtils::normalizeForSearch("Tiếng Việt") == "tieng viet");
  TEST_CHECK(StringUtils::normalizeForSearch("ΕΙΚΟΝΊΔΙΟ") == "εικονιδιο");
  TEST_CHECK(StringUtils::normalizeForSearch("Ёлка") == "елка");
  TEST_CHECK(StringUtils::normalizeForSearch("Straße") == StringUtils::normalizeForSearch("STRASSE"));
  // decomposed spelling
  TEST_CHECK(
      StringUtils::normalizeForSearch(
          "U\xCC\x88"
          "ber"
      )
      == "uber"
  );
  TEST_CHECK(StringUtils::normalizeForSearch("Animasyon Hızı") == "animasyon hizi");
  TEST_CHECK(StringUtils::normalizeForSearch("ANİMASYON HIZI") == "animasyon hizi");
  TEST_CHECK(StringUtils::normalizeForSearch("ガイド") == "ガイド");
  TEST_CHECK(StringUtils::normalizeForSearch("글꼴") == "글꼴");
  // invalid UTF-8 falls back to ASCII
  TEST_CHECK(
      StringUtils::normalizeForSearch(
          "\xFF"
          "ABC"
      )
      == "\xFF"
         "abc"
  );

  TEST_CHECK(StringUtils::splitShellWords("a  b\tc") == Words{"a", "b", "c"});
  TEST_CHECK(StringUtils::splitShellWords("'satty -f -'") == Words{"satty -f -"});
  TEST_CHECK(StringUtils::splitShellWords(R"("x\"y")") == Words{"x\"y"});
  TEST_CHECK(StringUtils::splitShellWords("a\\ b") == Words{"a b"});
  TEST_CHECK(StringUtils::splitShellWords("''") == Words{""});
  TEST_CHECK(!StringUtils::splitShellWords("'open").has_value());
  TEST_CHECK(StringUtils::splitShellWords(StringUtils::shellQuote("it's a \"t\"")) == Words{"it's a \"t\""});

  return 0;
}
