#pragma once

#include <cstdio>
#include <cstdlib>
#include <source_location>

#ifdef NDEBUG
#error "Noctalia test targets must be compiled without NDEBUG"
#endif

namespace noctalia::test {

  [[noreturn]] inline void
  failCheck(const char* expression, std::source_location location = std::source_location::current()) {
    std::fprintf(
        stderr, "%s:%u: check failed: %s\n", location.file_name(), static_cast<unsigned>(location.line()), expression
    );
    std::exit(EXIT_FAILURE);
  }

} // namespace noctalia::test

#define TEST_CHECK(expression)                                                                                         \
  do {                                                                                                                 \
    if (!(expression)) {                                                                                               \
      ::noctalia::test::failCheck(#expression);                                                                        \
    }                                                                                                                  \
  } while (false)
