#pragma once

#include <algorithm>
#include <source_location>

namespace util {

  namespace detail {
    // Logs an inverted-range clamp once per call site.
    void reportInvertedClamp(double lo, double hi, const std::source_location& loc);
  } // namespace detail

  // Like std::clamp but tolerant of an inverted range (hi < lo). std::clamp treats
  // hi < lo as undefined behavior, and a hardened libstdc++ (_GLIBCXX_ASSERTIONS,
  // which distro packagers enable) aborts on it. Use this only at layout/geometry
  // sites where adverse runtime data — a container narrower than its content, a
  // zero-size viewport — can momentarily invert the bounds. On inversion it returns
  // lo (the minimum wins, so content overflows rather than collapsing) and logs the
  // site once so the underlying math can be fixed. Do NOT use it to paper over a
  // true invariant violation: keep std::clamp where hi >= lo is guaranteed, so a
  // genuine bug still surfaces loudly.
  template <typename T>
  [[nodiscard]] T
  clampOrdered(const T& value, const T& lo, const T& hi, std::source_location loc = std::source_location::current()) {
    if (lo <= hi) {
      return std::clamp(value, lo, hi);
    }
    detail::reportInvertedClamp(static_cast<double>(lo), static_cast<double>(hi), loc);
    return lo;
  }

} // namespace util
