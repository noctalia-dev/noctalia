#pragma once

#include <cstdlib>
#include <exception>
#include <print>
#include <source_location>
#include <utility>

#ifdef NDEBUG
#error "Nythera test targets must be compiled without NDEBUG"
#endif

namespace nythera::test {

  struct CheckFailure {};

  [[noreturn]] inline void
  failCheck(const char* expression, std::source_location location = std::source_location::current()) {
    std::println(
        stderr, "{}:{}: check failed: {}", location.file_name(), static_cast<unsigned>(location.line()), expression
    );
    throw CheckFailure{};
  }

  template <typename Fn> int runTest(Fn&& fn) noexcept {
    try {
      std::forward<Fn>(fn)();
      return 0;
    } catch (const CheckFailure&) {
      return EXIT_FAILURE;
    } catch (const std::exception& error) {
      std::println(stderr, "Unhandled test exception: {}", error.what());
      return EXIT_FAILURE;
    } catch (...) {
      std::println(stderr, "Unhandled unknown test exception");
      return EXIT_FAILURE;
    }
  }

} // namespace nythera::test

#define TEST_CHECK(...)                                                                                                \
  do {                                                                                                                 \
    if (!(__VA_ARGS__)) {                                                                                              \
      ::nythera::test::failCheck(#__VA_ARGS__);                                                                        \
    }                                                                                                                  \
  } while (false)
