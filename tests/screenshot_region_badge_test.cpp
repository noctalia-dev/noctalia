// The dimensions badge of a region selection must not cover the area being captured. It sits
// above the selection, else below, else beside it, always on the surface, and only moves inside
// a selection that leaves no room on any side.

#include "capture/screenshot_region_badge.h"
#include "tests/test_check.h"

namespace {

  constexpr float kSurfaceW = 1920.0F;
  constexpr float kSurfaceH = 1080.0F;
  constexpr float kBadgeW = 80.0F;
  constexpr float kBadgeH = 26.0F;

  struct Rect {
    float x0 = 0.0F;
    float y0 = 0.0F;
    float x1 = 0.0F;
    float y1 = 0.0F;
  };

  Rect badgeFor(const Rect& sel, float surfaceW = kSurfaceW, float surfaceH = kSurfaceH) {
    const auto pos = capture::placeRegionBadge(surfaceW, surfaceH, sel.x0, sel.y0, sel.x1, sel.y1, kBadgeW, kBadgeH);
    return {.x0 = pos.x, .y0 = pos.y, .x1 = pos.x + kBadgeW, .y1 = pos.y + kBadgeH};
  }

  bool overlaps(const Rect& a, const Rect& b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }

  bool onSurface(const Rect& r, float surfaceW = kSurfaceW, float surfaceH = kSurfaceH) {
    return r.x0 >= 0.0F && r.y0 >= 0.0F && r.x1 <= surfaceW && r.y1 <= surfaceH;
  }

} // namespace

int main() {
  // Room above: the badge sits above the selection, aligned with its left edge.
  {
    const Rect sel{.x0 = 800.0F, .y0 = 500.0F, .x1 = 840.0F, .y1 = 530.0F};
    const Rect badge = badgeFor(sel);
    TEST_CHECK(badge.y1 < sel.y0);
    TEST_CHECK(badge.x0 == sel.x0);
  }

  // Selection against the top edge: below it.
  {
    const Rect sel{.x0 = 600.0F, .y0 = 0.0F, .x1 = 900.0F, .y1 = 200.0F};
    const Rect badge = badgeFor(sel);
    TEST_CHECK(badge.y0 > sel.y1);
    TEST_CHECK(badge.x0 == sel.x0);
  }

  // Full-height selection: to its right, or to its left when the right side is taken too.
  {
    const Rect sel{.x0 = 600.0F, .y0 = 0.0F, .x1 = 900.0F, .y1 = kSurfaceH};
    TEST_CHECK(badgeFor(sel).x0 > sel.x1);
    const Rect atRightEdge{.x0 = 1700.0F, .y0 = 0.0F, .x1 = kSurfaceW, .y1 = kSurfaceH};
    TEST_CHECK(badgeFor(atRightEdge).x1 < atRightEdge.x0);
  }

  // Corners: the badge stays outside the selection and on the surface.
  {
    constexpr Rect kCorners[] = {
        {.x0 = 0.0F, .y0 = 0.0F, .x1 = 60.0F, .y1 = 50.0F},
        {.x0 = 1860.0F, .y0 = 0.0F, .x1 = kSurfaceW, .y1 = 50.0F},
        {.x0 = 0.0F, .y0 = 1030.0F, .x1 = 60.0F, .y1 = kSurfaceH},
        {.x0 = 1860.0F, .y0 = 1030.0F, .x1 = kSurfaceW, .y1 = kSurfaceH},
    };
    for (const Rect& sel : kCorners) {
      const Rect badge = badgeFor(sel);
      TEST_CHECK(!overlaps(badge, sel));
      TEST_CHECK(onSurface(badge));
    }
  }

  // No room on any side: inside the selection, still on the surface.
  {
    const Rect sel{.x0 = 0.0F, .y0 = 0.0F, .x1 = kSurfaceW, .y1 = kSurfaceH};
    const Rect badge = badgeFor(sel);
    TEST_CHECK(overlaps(badge, sel));
    TEST_CHECK(onSurface(badge));
  }

  // Any selection, on a large and on a small surface: the badge is on the surface, and it only
  // overlaps a selection that leaves less than the badge's size free on all four sides.
  constexpr float kSurfaces[][2] = {{kSurfaceW, kSurfaceH}, {640.0F, 360.0F}, {1280.0F, 720.5F}};
  for (const auto& surface : kSurfaces) {
    const float surfaceW = surface[0];
    const float surfaceH = surface[1];
    constexpr float kStep = 37.0F;
    for (float x0 = 0.0F; x0 < surfaceW; x0 += kStep) {
      for (float y0 = 0.0F; y0 < surfaceH; y0 += kStep) {
        for (float w = 1.0F; x0 + w <= surfaceW; w = (w * 3.0F) + 5.0F) {
          for (float h = 1.0F; y0 + h <= surfaceH; h = (h * 3.0F) + 5.0F) {
            const Rect sel{.x0 = x0, .y0 = y0, .x1 = x0 + w, .y1 = y0 + h};
            const Rect badge = badgeFor(sel, surfaceW, surfaceH);
            TEST_CHECK(onSurface(badge, surfaceW, surfaceH));
            const bool roomOutside = sel.y0 >= kBadgeH * 2.0F
                || surfaceH - sel.y1 >= kBadgeH * 2.0F
                || sel.x0 >= kBadgeW * 2.0F
                || surfaceW - sel.x1 >= kBadgeW * 2.0F;
            if (roomOutside) {
              TEST_CHECK(!overlaps(badge, sel));
            }
          }
        }
      }
    }
  }

  return 0;
}
