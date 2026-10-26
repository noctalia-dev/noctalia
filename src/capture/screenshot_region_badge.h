#pragma once

namespace capture {

  struct RegionBadgePosition {
    float x = 0.0F;
    float y = 0.0F;
  };

  // Top-left corner for the dimensions badge of a region selection, in surface-local coordinates.
  // The badge stays off the selection so it never covers what is being captured: above it, else
  // below it, else to its right, else to its left. A selection that leaves no room on any side
  // fills almost the whole surface, and only then does the badge sit inside its top-left corner.
  // `sel*` is the outer edge of the selection outline, clipped to the surface.
  [[nodiscard]] RegionBadgePosition placeRegionBadge(
      float surfaceW, float surfaceH, float selX0, float selY0, float selX1, float selY1, float badgeW, float badgeH
  );

} // namespace capture
