// A flush dock only overlaps the screen edge on a fractionally scaled output, where the
// compositor can round the edge a device pixel short. At an integer scale the overlap would
// clip the panel's own edge row, so the surface must sit exactly on the boundary.

#include "config/config_types.h"
#include "shell/dock/dock_geometry.h"
#include "tests/test_check.h"
#include "wayland/surface.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace {

  constexpr std::size_t kItemCount = 4;

  DockConfig flushDock(DockEdge edge) {
    DockConfig cfg;
    cfg.position = edge;
    cfg.marginEdge = 0;
    cfg.shadow = false;
    return cfg;
  }

  shell::dock::DockSurfaceGeometry geometryFor(const DockConfig& cfg, bool fractionalScale) {
    const ShellConfig::ShadowConfig shadow;
    return shell::dock::computeSurfaceGeometry(cfg, shadow, kItemCount, fractionalScale, 1920, 1080);
  }

  int edgeMargin(const shell::dock::DockSurfaceGeometry& geometry, DockEdge edge) {
    switch (edge) {
    case DockEdge::Bottom:
      return geometry.marginBottom;
    case DockEdge::Top:
      return geometry.marginTop;
    case DockEdge::Left:
      return geometry.marginLeft;
    case DockEdge::Right:
      return geometry.marginRight;
    }
    return 0;
  }

} // namespace

int main() {
  constexpr DockEdge kEdges[] = {DockEdge::Bottom, DockEdge::Top, DockEdge::Left, DockEdge::Right};

  for (const DockEdge edge : kEdges) {
    const DockConfig cfg = flushDock(edge);
    TEST_CHECK(edgeMargin(geometryFor(cfg, /*fractionalScale=*/false), edge) == 0);
    TEST_CHECK(edgeMargin(geometryFor(cfg, /*fractionalScale=*/true), edge) == -1);

    // Surface size and reserved space describe the dock itself and must not move with the
    // overlap, or the dock would reserve a pixel it does not paint.
    const auto integerGeometry = geometryFor(cfg, /*fractionalScale=*/false);
    const auto fractionalGeometry = geometryFor(cfg, /*fractionalScale=*/true);
    TEST_CHECK(integerGeometry.surfaceW == fractionalGeometry.surfaceW);
    TEST_CHECK(integerGeometry.surfaceH == fractionalGeometry.surfaceH);
    TEST_CHECK(integerGeometry.exclusiveZone == fractionalGeometry.exclusiveZone);
  }

  // A dock held off the edge by a margin is already clear of the rounding, so it never overlaps.
  for (const DockEdge edge : kEdges) {
    DockConfig cfg = flushDock(edge);
    cfg.marginEdge = 8;
    TEST_CHECK(edgeMargin(geometryFor(cfg, /*fractionalScale=*/true), edge) >= 0);
  }

  // The hidden auto-hide trigger strip keeps its on-screen thickness: the part of the surface
  // pushed past the output edge cannot be hovered.
  {
    DockConfig cfg = flushDock(DockEdge::Bottom);
    cfg.autoHide = true;
    const auto integerRegion =
        shell::dock::computeInputRegion(cfg, shell::dock::DockPanelGeometry{}, 200, 60, true, false);
    const auto fractionalRegion =
        shell::dock::computeInputRegion(cfg, shell::dock::DockPanelGeometry{}, 200, 60, true, true);
    TEST_CHECK(integerRegion.size() == 1);
    TEST_CHECK(fractionalRegion.size() == 1);
    TEST_CHECK(fractionalRegion[0].height == integerRegion[0].height + 1);
    TEST_CHECK(fractionalRegion[0].y == integerRegion[0].y - 1);
  }

  // Overflowing docks stop at the output ends so their item viewport can scroll.
  for (const DockEdge edge : kEdges) {
    DockConfig cfg = flushDock(edge);
    cfg.marginEnds = 12;
    const ShellConfig::ShadowConfig shadow;
    const auto geometry = shell::dock::computeSurfaceGeometry(
        cfg, shadow, 100, false, /*outputLogicalWidth=*/800, /*outputLogicalHeight=*/600
    );
    const std::uint32_t expectedLength = shell::dock::isVerticalEdge(edge) ? 576U : 776U;
    const std::uint32_t actualLength = shell::dock::isVerticalEdge(edge) ? geometry.surfaceH : geometry.surfaceW;
    TEST_CHECK(actualLength == expectedLength);
  }

  for (bool panelShadow : {false, true}) {
    for (const DockEdge edge : kEdges) {
      for (bool magnification : {false, true}) {
        for (float base : {0.85F, 1.0F, 1.75F}) {
          for (int padding : {0, 8}) {
            for (int margin : {0, 40}) {
              DockConfig cfg = flushDock(edge);
              cfg.shadow = panelShadow;
              cfg.iconSize = 54;
              cfg.mainAxisPadding = cfg.crossAxisPadding = padding;
              cfg.activeScale = base;
              cfg.inactiveScale = std::min(base, 1.0F);
              cfg.magnification = magnification;
              cfg.magnificationScale = 2.0F;
              cfg.marginEdge = margin;
              cfg.reserveSpace = true;
              cfg.showInstanceCount = false;
              const ShellConfig::ShadowConfig shadow;
              const auto before = shell::dock::computeSurfaceGeometry(cfg, shadow, 1, false, 1920, 1080);
              cfg.iconShadow = true;
              const auto surface = shell::dock::computeSurfaceGeometry(cfg, shadow, 1, false, 1920, 1080);
              const auto panel = shell::dock::computePanelGeometry(cfg, shadow, surface.surfaceW, surface.surfaceH);
              TEST_CHECK(before.exclusiveZone == surface.exclusiveZone);
              const bool vertical = shell::dock::isVerticalEdge(edge);
              TEST_CHECK(
                  panel.panelW == (vertical ? shell::dock::dockThickness(cfg) : shell::dock::dockContentSize(cfg, 1))
              );
              TEST_CHECK(
                  panel.panelH == (vertical ? shell::dock::dockContentSize(cfg, 1) : shell::dock::dockThickness(cfg))
              );
              const float scale = base * (magnification ? 2.0F : 1.0F);
              const float growth = cfg.iconSize * (scale - 1.0F) * 0.5F;
              float x = panel.panelX + padding + 6;
              float y = panel.panelY + padding + 6;
              if (magnification && scale > 1) {
                shell::dock::shiftAlongEdge(edge, x, y, -growth);
              }
              const float left = x - growth - 6 * scale;
              const float right = x - growth + (cfg.iconSize + 6) * scale;
              const float top = y - growth - 4 * scale;
              const float bottom = y - growth + (cfg.iconSize + 8) * scale;
              if (margin > 0 || edge != DockEdge::Left)
                TEST_CHECK(left >= -0.01F);
              if (margin > 0 || edge != DockEdge::Right)
                TEST_CHECK(right <= surface.surfaceW + 0.01F);
              if (margin > 0 || edge != DockEdge::Top)
                TEST_CHECK(top >= -0.01F);
              if (margin > 0 || edge != DockEdge::Bottom)
                TEST_CHECK(bottom <= surface.surfaceH + 0.01F);
              const auto [dx, dy] =
                  shell::dock::computeHiddenSlideDelta(cfg, shadow, surface.surfaceW, surface.surfaceH, panel);
              if (edge == DockEdge::Left)
                TEST_CHECK(right + dx <= 0);
              if (edge == DockEdge::Right)
                TEST_CHECK(left + dx >= surface.surfaceW);
              if (edge == DockEdge::Top)
                TEST_CHECK(bottom + dy <= 0);
              if (edge == DockEdge::Bottom)
                TEST_CHECK(top + dy >= surface.surfaceH);
              const auto input =
                  shell::dock::computeInputRegion(cfg, panel, surface.surfaceW, surface.surfaceH, false, false);
              TEST_CHECK(input.size() == 1);
              TEST_CHECK(input[0].width == panel.panelW && input[0].height == panel.panelH);
            }
          }
        }
      }
    }
  }

  return 0;
}
