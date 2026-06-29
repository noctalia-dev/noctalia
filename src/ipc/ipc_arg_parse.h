#pragma once

#include "util/string_utils.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noctalia::ipc {

  inline std::vector<std::string> splitWords(std::string_view text) { return StringUtils::splitWhitespace(text); }

  inline std::optional<float> parseNormalizedOrPercent(std::string_view token, float maxPercent = 100.0f) {
    std::string value(token);
    bool isPercent = false;
    if (!value.empty() && value.back() == '%') {
      isPercent = true;
      value.pop_back();
    }
    if (value.empty()) {
      return std::nullopt;
    }

    const auto parsed = StringUtils::parseDotDecimal<float>(value);
    if (!parsed.has_value()) {
      return std::nullopt;
    }
    const float amount = *parsed;

    if (isPercent || !value.contains('.')) {
      if (amount < 0.0f || amount > maxPercent) {
        return std::nullopt;
      }
      return amount / 100.0f;
    }

    if (amount >= 0.0f && amount <= 1.0f) {
      return amount;
    }
    if (amount > 1.0f && amount <= maxPercent) {
      return amount / 100.0f;
    }

    return std::nullopt;
  }

} // namespace noctalia::ipc
