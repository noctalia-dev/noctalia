#include "util/string_utils.h"

#include <cstdio>
#include <print>
#include <string_view>

namespace {

  bool expectEqual(std::string_view actual, std::string_view expected, const char* message) {
    if (actual != expected) {
      std::println(
          stderr, "string_utils_test: {}: expected '{:.{}}', got '{:.{}}'", message, expected.data(),
          static_cast<int>(expected.size()), actual.data(), static_cast<int>(actual.size())
      );
      return false;
    }
    return true;
  }

  bool expectCompare(int actual, int expectedSign, const char* message) {
    const int sign = (actual > 0) - (actual < 0);
    if (sign != expectedSign) {
      std::println(
          stderr, "string_utils_test: {}: expected sign {}, got {} (raw {})", message, expectedSign, sign, actual
      );
      return false;
    }
    return true;
  }

} // namespace

int main() {
  bool ok = true;
  ok = expectEqual(StringUtils::urlEncode("abc-_.~09AZaz"), "abc-_.~09AZaz", "keeps unreserved characters") && ok;
  ok = expectEqual(StringUtils::urlEncode("hello world"), "hello%20world", "encodes spaces as percent escapes") && ok;
  ok = expectEqual(
           StringUtils::urlEncode("dir/file?x=1&y=%"), "dir%2Ffile%3Fx%3D1%26y%3D%25", "encodes reserved URL characters"
       )
      && ok;
  ok = expectEqual(StringUtils::urlEncode("\xC3\xA9"), "%C3%A9", "encodes non-ASCII bytes") && ok;

  ok = expectCompare(
           StringUtils::naturalCaseInsensitiveCompare("image(2)", "image(10)"), -1, "natural: image(2) < image(10)"
       )
      && ok;
  ok = expectCompare(
           StringUtils::naturalCaseInsensitiveCompare("image(10)", "image(2)"), 1, "natural: image(10) > image(2)"
       )
      && ok;
  ok = expectCompare(
           StringUtils::naturalCaseInsensitiveCompare("Image(1)", "image(1)"), 0, "natural: case-insensitive equality"
       )
      && ok;
  ok = expectCompare(StringUtils::naturalCaseInsensitiveCompare("a2", "a10"), -1, "natural: a2 < a10") && ok;
  ok = expectCompare(StringUtils::naturalCaseInsensitiveCompare("file", "file1"), -1, "natural: shorter prefix first")
      && ok;

  return ok ? 0 : 1;
}
