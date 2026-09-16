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
    return shell::dock::computeSurfaceGeometry(cfg, shadow, kItemCount, fractionalScale);
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

  return 0;
}
