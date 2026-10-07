#include "launcher/math_provider.h"

#include <cstdio>
#include <print>
#include <string_view>

namespace {

  bool expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "math_provider_test: {}", message);
    }
    return condition;
  }

} // namespace

int main() {
  MathProvider provider(nullptr, nullptr, nullptr);
  provider.initialize();

  bool ok = true;
  ok = expect(provider.query("EUR").empty(), "digit-free global query should be filtered") && ok;
  ok = expect(!provider.queryPrefixed("EUR").empty(), "digit-free prefixed query should be evaluated") && ok;
  ok = expect(!provider.query("2 + 2").empty(), "numeric global query should be evaluated") && ok;

  return ok ? 0 : 1;
}
