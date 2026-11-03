#pragma once

#include <limits>
#include <string_view>

namespace FuzzyMatch {

  inline constexpr double noMatchScore = -std::numeric_limits<double>::infinity();

  inline bool isMatch(double value) noexcept { return value > noMatchScore; }

  double score(std::string_view pattern, std::string_view text);

} // namespace FuzzyMatch
