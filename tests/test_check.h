#pragma once

#include <cmath>
#include <cstdlib>
#include <optional>
#include <print>
#include <source_location>
#include <type_traits>

#ifdef NDEBUG
#error "Noctalia test targets must be compiled without NDEBUG"
#endif

namespace noctalia::test {

  [[noreturn]] inline void
  failCheck(const char* expression, std::source_location location = std::source_location::current()) {
    std::println(
        stderr, "{}:{}: check failed: {}", location.file_name(), static_cast<unsigned>(location.line()), expression
    );
    std::exit(EXIT_FAILURE);
  }

  // A function rather than `if (!(expression))` in the macro, so clang-tidy does not ask to apply
  // De Morgan to every check over a compound condition.
  inline void
  check(bool passed, const char* expression, std::source_location location = std::source_location::current()) {
    if (!passed) {
      failCheck(expression, location);
    }
  }

  template <typename T> inline constexpr bool kIsOptional = false;
  template <typename T> inline constexpr bool kIsOptional<std::optional<T>> = true;

  // An empty optional fails the check; otherwise its value is compared.
  template <typename Actual, typename Expected, typename Tolerance>
  void checkNear(
      const Actual& actual, const Expected& expected, const Tolerance& tolerance, const char* expression,
      std::source_location location = std::source_location::current()
  ) {
    if constexpr (kIsOptional<std::remove_cvref_t<Actual>>) {
      if (!actual.has_value()) {
        std::println(
            stderr, "{}:{}: check failed: {}: no value", location.file_name(), static_cast<unsigned>(location.line()),
            expression
        );
        std::exit(EXIT_FAILURE);
      }
      checkNear(*actual, expected, tolerance, expression, location);
    } else {
      if (!(std::abs(actual - expected) <= tolerance)) {
        std::println(
            stderr, "{}:{}: check failed: {}: got {}, expected {} within {}", location.file_name(),
            static_cast<unsigned>(location.line()), expression, actual, expected, tolerance
        );
        std::exit(EXIT_FAILURE);
      }
    }
  }

} // namespace noctalia::test

// Variadic so a check can contain unparenthesized commas (designated initializers, template arguments).
#define TEST_CHECK(...) ::noctalia::test::check(static_cast<bool>(__VA_ARGS__), #__VA_ARGS__)

#define TEST_NEAR(actual, expected, tolerance)                                                                         \
  ::noctalia::test::checkNear(                                                                                         \
      (actual), (expected), (tolerance), "TEST_NEAR(" #actual ", " #expected ", " #tolerance ")"                       \
  )
