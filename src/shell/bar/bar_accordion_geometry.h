#pragma once

#include <algorithm>
#include <cmath>

[[nodiscard]] inline float barAccordionRevealExtent(float collapsed, float expanded, float progress) {
  // Match Flex's logical-pixel grid so end-aligned neighbours stay stationary.
  // Round endpoints up to keep fractional-width content inside the clip.
  const float from = std::ceil(collapsed);
  const float to = std::max(from, std::ceil(expanded));
  return from + std::round((to - from) * std::clamp(progress, 0.0F, 1.0F));
}
