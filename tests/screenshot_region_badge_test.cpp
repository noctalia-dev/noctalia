// The dimensions badge of a region selection must not cover the area being captured. It sits
// above the selection, else below, else beside it, always on the surface, and only moves inside
// a selection that leaves no room on any side. The geometry helpers pinned here report the
// saved file's dimensions.

#include "capture/screenshot_geometry.h"
#include "capture/screenshot_region_badge.h"
#include "tests/test_check.h"

#include <cmath>
#include <limits>

namespace {

  constexpr float kSurfaceW = 1920.0F;
  constexpr double kEps = std::numeric_limits<double>::epsilon();
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

  // Device extents: the crop's floor/ceil/clamp bounds.
  {
    TEST_CHECK(capture::deviceSelectionExtent(10, 101, 1.0, 4096) == 91);
    TEST_CHECK(capture::deviceSelectionExtent(10, 101, 2.0, 4096) == 182);
    TEST_CHECK(capture::deviceSelectionExtent(3, 100, 2.25, 4096) == 219);
    TEST_CHECK(capture::deviceSelectionExtent(0, 100, 1.5, 120) == 120); // clamped to frame extent
  }

  // Ratios follow the crop's derivation, and a full-screen selection reads 3840x2160 at 225%.
  {
    const double cosmicStyle = capture::devicePixelsPerLogicalUnit(3840, 1707, 2.25);
    TEST_CHECK(capture::deviceSelectionExtent(0, 1707, cosmicStyle, 3840) == 3840);
    TEST_CHECK(
        capture::deviceSelectionExtent(0, 960, capture::devicePixelsPerLogicalUnit(2160, 960, 2.25), 2160) == 2160
    );
    // Viewporter-scaled integer outputs (buffer scale stays 1): 2 from mode/logical.
    TEST_CHECK(std::abs(capture::devicePixelsPerLogicalUnit(3840, 1920, 1.0) - 2.0) <= kEps);
    // No mode/logical pair: fall back to the resolved scale, at least 1.
    TEST_CHECK(std::abs(capture::devicePixelsPerLogicalUnit(0, 1920, 2.0) - 2.0) <= kEps);
    TEST_CHECK(std::abs(capture::devicePixelsPerLogicalUnit(0, 1920, 0.0) - 1.0) <= kEps);
  }

  // Canvas size mirrors composeGlobalRegion: single piece passes through, sharpest output ratio wins.
  {
    const capture::RegionPieceRatio single{
        .cropWidth = 901, .cropHeight = 676, .outputWidth = 3840, .outputLogicalWidth = 1706
    };
    TEST_CHECK(capture::regionCanvasSize(400, 300, &single, 1).width == 901);
    TEST_CHECK(capture::regionCanvasSize(400, 300, &single, 1).height == 676);

    // Sharpest true ratio over outputs and axes sets the density, here 3840/1706 on the horizontal axis.
    const capture::RegionPieceRatio mixed[] = {
        {.cropWidth = 901,
         .cropHeight = 676,
         .outputWidth = 3840,
         .outputLogicalWidth = 1706,
         .outputHeight = 2160,
         .outputLogicalHeight = 960},
        {.cropWidth = 100,
         .cropHeight = 100,
         .outputWidth = 3840,
         .outputLogicalWidth = 1706,
         .outputHeight = 2160,
         .outputLogicalHeight = 960},
    };
    const auto canvas = capture::regionCanvasSize(500, 700, mixed, 2);
    TEST_CHECK(canvas.width == 1126);  // ceil(500 * 2.250879)
    TEST_CHECK(canvas.height == 1576); // ceil(700 * 2.250879)

    // Mixed-scale desktop: the sharpest true ratio wins, and the 100% output is upscaled to it.
    const capture::RegionPieceRatio vertical[] = {
        {.cropWidth = 100,
         .cropHeight = 100,
         .outputWidth = 1920,
         .outputLogicalWidth = 1920,
         .outputHeight = 1080,
         .outputLogicalHeight = 1080,
         .outputScale = 1.0},
        {.cropWidth = 200,
         .cropHeight = 400,
         .outputWidth = 3840,
         .outputLogicalWidth = 1920,
         .outputHeight = 2160,
         .outputLogicalHeight = 1080,
         .outputScale = 2.0},
    };
    const auto tall = capture::regionCanvasSize(300, 300, vertical, 2);
    TEST_CHECK(tall.width == 600);
    TEST_CHECK(tall.height == 600);

    // Seam case: a selection ending 1 px past a 225% seam keeps the outputs' own 3840/1706
    // density instead of the 1 px piece's ratio.
    const capture::RegionPieceRatio seam[] = {
        {.cropWidth = 198,
         .cropHeight = 73,
         .outputWidth = 3840,
         .outputLogicalWidth = 1706,
         .outputHeight = 2160,
         .outputLogicalHeight = 960},
        {.cropWidth = 3,
         .cropHeight = 3,
         .outputWidth = 3840,
         .outputLogicalWidth = 1706,
         .outputHeight = 2160,
         .outputLogicalHeight = 960},
    };
    const auto fixedCanvas = capture::regionCanvasSize(89, 32, seam, 2);
    TEST_CHECK(fixedCanvas.width == 201); // ceil(89 * 2.250879)
    TEST_CHECK(fixedCanvas.height == 73); // ceil(32 * 2.250879)

    // Empty selection over no outputs: the canvas size is zero.
    TEST_CHECK(capture::regionCanvasSize(10, 10, nullptr, 0).width == 0);
  }

  return 0;
}
