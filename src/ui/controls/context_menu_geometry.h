#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace context_menu_geometry {
  struct Columns {
    float labelWidth;
    float shortcutX;
    float shortcutWidth;
  };

  // Measured text widths feed the same layout used by native rows. The shortcut
  // ends at the common right edge, independently of action-name length.
  [[nodiscard]] inline Columns
  columns(float rowWidth, float textStart, float rightInset, float measuredShortcutWidth, float shortcutGap) noexcept {
    const float right = std::max(textStart, rowWidth - rightInset);
    const float available = std::max(0.0F, right - textStart);
    const float width = std::clamp(measuredShortcutWidth, 0.0F, available);
    const float gap = width > 0.0F ? std::clamp(shortcutGap, 0.0F, available - width) : 0.0F;
    return {available - width - gap, right - width, width};
  }

  [[nodiscard]] inline float fitHeight(float preferredHeight, float surfaceHeight, float shadowHeight) noexcept {
    return std::min(preferredHeight, std::max(1.0F, surfaceHeight - shadowHeight));
  }

  // Number of leading rows that fit in a content viewport. Each row height already
  // includes content scale; outerPadding is both top and bottom menu padding.
  [[nodiscard]] inline std::size_t visibleRows(
      const std::vector<float>& rowHeights, std::size_t requestedRows, float viewportHeight, float outerPadding,
      float rowGap = 0.0F
  ) noexcept {
    const std::size_t limit = std::min(rowHeights.size(), std::max<std::size_t>(1, requestedRows));
    float used = std::max(0.0F, outerPadding) * 2.0F;
    std::size_t count = 0;
    for (; count < limit; ++count) {
      const float gap = count == 0 ? 0.0F : std::max(0.0F, rowGap);
      if (used + gap + rowHeights[count] > viewportHeight)
        break;
      used += gap;
      used += rowHeights[count];
    }
    return std::max<std::size_t>(1, count);
  }

} // namespace context_menu_geometry
