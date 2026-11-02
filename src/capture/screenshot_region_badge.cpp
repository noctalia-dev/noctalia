#include "capture/screenshot_region_badge.h"

#include <algorithm>

namespace capture {
  namespace {

    constexpr float kBadgeGap = 8.0F;
    constexpr float kBadgeSurfaceMargin = 4.0F;

  } // namespace

  RegionBadgePosition placeRegionBadge(
      float surfaceW, float surfaceH, float selX0, float selY0, float selX1, float selY1, float badgeW, float badgeH
  ) {
    const float minX = kBadgeSurfaceMargin;
    const float minY = kBadgeSurfaceMargin;
    const float maxX = std::max(minX, surfaceW - badgeW - kBadgeSurfaceMargin);
    const float maxY = std::max(minY, surfaceH - badgeH - kBadgeSurfaceMargin);
    const float alignedX = std::clamp(selX0, minX, maxX);
    const float alignedY = std::clamp(selY0, minY, maxY);

    const float aboveY = selY0 - kBadgeGap - badgeH;
    if (aboveY >= minY) {
      return {.x = alignedX, .y = aboveY};
    }
    const float belowY = selY1 + kBadgeGap;
    if (belowY + badgeH + kBadgeSurfaceMargin <= surfaceH) {
      return {.x = alignedX, .y = belowY};
    }
    const float rightX = selX1 + kBadgeGap;
    if (rightX + badgeW + kBadgeSurfaceMargin <= surfaceW) {
      return {.x = rightX, .y = alignedY};
    }
    const float leftX = selX0 - kBadgeGap - badgeW;
    if (leftX >= minX) {
      return {.x = leftX, .y = alignedY};
    }
    return {
        .x = std::clamp(selX0 + kBadgeGap, minX, maxX),
        .y = std::clamp(selY0 + kBadgeGap, minY, maxY),
    };
  }

} // namespace capture
