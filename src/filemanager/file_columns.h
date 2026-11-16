#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>

namespace filemanager {

  // Widths are logical UI pixels, independent of window scaling. The Name width is
  // the flexible first column; Size and Modified are the trailing columns.
  struct FileColumns {
    float name = 0.0F;
    float size = 96.0F;
    float modified = 152.0F;
  };

  inline constexpr float kNameColumnMin = 120.0F;
  inline constexpr float kSizeColumnMin = 72.0F;
  inline constexpr float kModifiedColumnMin = 112.0F;

  inline FileColumns fitFileColumns(FileColumns widths, float available) {
    available = std::max(0.0F, available);
    const float minTotal = kNameColumnMin + kSizeColumnMin + kModifiedColumnMin;
    if (available < minTotal) {
      const float ratio = available / minTotal;
      return {kNameColumnMin * ratio, kSizeColumnMin * ratio, kModifiedColumnMin * ratio};
    }
    widths.size = std::max(kSizeColumnMin, widths.size);
    widths.modified = std::max(kModifiedColumnMin, widths.modified);
    const float trailingMax = available - kNameColumnMin;
    if (widths.size + widths.modified > trailingMax) {
      const float excess = widths.size + widths.modified - trailingMax;
      const float shrinkable = (widths.size - kSizeColumnMin) + (widths.modified - kModifiedColumnMin);
      if (shrinkable > 0.0F) {
        const float ratio = std::min(1.0F, excess / shrinkable);
        widths.size -= (widths.size - kSizeColumnMin) * ratio;
        widths.modified -= (widths.modified - kModifiedColumnMin) * ratio;
      }
    }
    widths.name = available - widths.size - widths.modified;
    return widths;
  }

  inline FileColumns resizeFileColumnBoundary(FileColumns widths, unsigned boundary, float delta, float available) {
    widths = fitFileColumns(widths, available);
    const float minTotal = kNameColumnMin + kSizeColumnMin + kModifiedColumnMin;
    if (available < minTotal || !std::isfinite(delta))
      return widths;
    if (boundary == 0) {
      const float next = std::clamp(widths.size - delta, kSizeColumnMin, available - widths.modified - kNameColumnMin);
      widths.size = next;
    } else if (boundary == 1) {
      const float next = std::clamp(widths.size + delta, kSizeColumnMin, available - widths.name - kModifiedColumnMin);
      widths.size = next;
      widths.modified = available - widths.name - widths.size;
    }
    if (boundary == 0)
      widths.name = available - widths.size - widths.modified;
    return widths;
  }

  inline std::string serializeFileColumns(FileColumns widths) {
    return std::to_string(widths.name) + "," + std::to_string(widths.size) + "," + std::to_string(widths.modified);
  }

  inline bool parseFileColumns(std::string_view text, FileColumns& widths) {
    float values[3]{};
    for (std::size_t i = 0; i < 3; ++i) {
      const auto comma = text.find(',');
      const auto token = text.substr(0, comma);
      if (token.empty())
        return false;
      auto& value = values[i];
      const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
      if (error != std::errc{}
          || end != token.data() + token.size()
          || !std::isfinite(value)
          || value <= 0.0F
          || value > 10000.0F)
        return false;
      if (i < 2) {
        if (comma == std::string_view::npos)
          return false;
        text.remove_prefix(comma + 1);
      } else if (comma != std::string_view::npos)
        return false;
    }
    widths = {values[0], values[1], values[2]};
    return true;
  }

} // namespace filemanager
