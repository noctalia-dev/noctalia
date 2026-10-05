// A flush dock only overlaps the screen edge on a fractionally scaled output, where the
// compositor can round the edge a device pixel short. At an integer scale the overlap would
// clip the panel's own edge row, so the surface must sit exactly on the boundary.

#include "config/config_types.h"
#include "shell/dock/dock_geometry.h"
#include "tests/test_check.h"
#include "wayland/surface.h"

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

  // Overflowing docks stop at the margin_ends inset so their item viewport can scroll.
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

  {
    DockConfig cfg = flushDock(DockEdge::Bottom);
    cfg.maximize = true;
    cfg.marginEnds = 80;
    const ShellConfig::ShadowConfig shadow;
    const auto geometry = shell::dock::computeSurfaceGeometry(
        cfg, shadow, kItemCount, false, /*outputLogicalWidth=*/1920, /*outputLogicalHeight=*/1080
    );
    const auto panel = shell::dock::computePanelGeometry(cfg, shadow, geometry.surfaceW, geometry.surfaceH);
    const auto concave = shell::dock::dockConcaveShape(cfg);
    TEST_CHECK(geometry.surfaceW == 1760U);
    TEST_CHECK(geometry.marginLeft == 0);
    TEST_CHECK(geometry.marginRight == 0);
    TEST_CHECK(panel.panelX - concave.logicalInset.left == 0.0F);
    TEST_CHECK(panel.panelX + panel.panelW + concave.logicalInset.right == 1760.0F);
    const auto input = shell::dock::computeInputRegion(cfg, panel, 1760, 1080, false, false);
    TEST_CHECK(input.size() == 1);
    TEST_CHECK(input.front().x == 0);
    TEST_CHECK(input.front().width == 1760);
  }

  {
    DockConfig cfg = flushDock(DockEdge::Left);
    cfg.maximize = true;
    cfg.marginEnds = 80;
    const ShellConfig::ShadowConfig shadow;
    const auto geometry = shell::dock::computeSurfaceGeometry(
        cfg, shadow, kItemCount, false, /*outputLogicalWidth=*/1920, /*outputLogicalHeight=*/1080
    );
    const auto panel = shell::dock::computePanelGeometry(cfg, shadow, geometry.surfaceW, geometry.surfaceH);
    const auto concave = shell::dock::dockConcaveShape(cfg);
    TEST_CHECK(geometry.surfaceH == 920U);
    TEST_CHECK(geometry.marginTop == 0);
    TEST_CHECK(geometry.marginBottom == 0);
    TEST_CHECK(panel.panelY - concave.logicalInset.top == 0.0F);
    TEST_CHECK(panel.panelY + panel.panelH + concave.logicalInset.bottom == 920.0F);
    const auto input = shell::dock::computeInputRegion(cfg, panel, 1920, 920, false, false);
    TEST_CHECK(input.size() == 1);
    TEST_CHECK(input.front().y == 0);
    TEST_CHECK(input.front().height == 920);
  }

  return 0;
}
