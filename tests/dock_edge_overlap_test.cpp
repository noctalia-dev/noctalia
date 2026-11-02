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
    return shell::dock::computeSurfaceGeometry(cfg, shadow, fractionalScale, 1920, 1080);
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

  bool panelTouchesSurfaceEdge(
      const shell::dock::DockPanelGeometry& panel, const shell::dock::DockSurfaceGeometry& surface, DockEdge edge
  ) {
    switch (edge) {
    case DockEdge::Bottom:
      return panel.panelY + panel.panelH == static_cast<float>(surface.surfaceH);
    case DockEdge::Top:
      return panel.panelY == 0.0F;
    case DockEdge::Left:
      return panel.panelX == 0.0F;
    case DockEdge::Right:
      return panel.panelX + panel.panelW == static_cast<float>(surface.surfaceW);
    }
    return false;
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

  // Magnification reserves surface space away from the screen edge. Instance-count badges face
  // inward, so enabling them must not move a flush panel away from its anchored edge.
  for (const DockEdge edge : kEdges) {
    DockConfig cfg = flushDock(edge);
    cfg.magnification = true;
    cfg.magnificationScale = 1.45F;
    cfg.showInstanceCount = true;
    const auto surface = geometryFor(cfg, /*fractionalScale=*/false);
    const ShellConfig::ShadowConfig shadow;
    const auto panel = shell::dock::computePanelGeometry(
        cfg, shadow, static_cast<float>(surface.surfaceW), static_cast<float>(surface.surfaceH), kItemCount
    );
    TEST_CHECK(panelTouchesSurfaceEdge(panel, surface, edge));
  }

  {
    DockConfig top = flushDock(DockEdge::Top);
    top.magnification = true;
    top.showInstanceCount = true;
    DockConfig bottom = top;
    bottom.position = DockEdge::Bottom;
    TEST_CHECK(
        geometryFor(top, /*fractionalScale=*/false).surfaceH == geometryFor(bottom, /*fractionalScale=*/false).surfaceH
    );
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

  // The surface spans the output's main axis minus the end margins, independent of item count, so
  // window changes never resize the layer surface and overflowing docks scroll inside it.
  for (const DockEdge edge : kEdges) {
    DockConfig cfg = flushDock(edge);
    cfg.marginEnds = 12;
    const ShellConfig::ShadowConfig shadow;
    const auto geometry = shell::dock::computeSurfaceGeometry(
        cfg, shadow, false, /*outputLogicalWidth=*/800, /*outputLogicalHeight=*/600
    );
    const std::uint32_t expectedLength = shell::dock::isVerticalEdge(edge) ? 576U : 776U;
    const std::uint32_t actualLength = shell::dock::isVerticalEdge(edge) ? geometry.surfaceH : geometry.surfaceW;
    TEST_CHECK(actualLength == expectedLength);
  }

  // The capsule is centered in the static surface and sized by its item count.
  {
    DockConfig cfg = flushDock(DockEdge::Bottom);
    cfg.marginEnds = 10;
    const ShellConfig::ShadowConfig shadow;
    const auto geo = shell::dock::computeSurfaceGeometry(cfg, shadow, false, 1920, 1080);
    const auto panel1 = shell::dock::computePanelGeometry(
        cfg, shadow, static_cast<float>(geo.surfaceW), static_cast<float>(geo.surfaceH), 1
    );
    const auto panel5 = shell::dock::computePanelGeometry(
        cfg, shadow, static_cast<float>(geo.surfaceW), static_cast<float>(geo.surfaceH), 5
    );
    const float center1 = panel1.panelX + panel1.panelW * 0.5F;
    const float center5 = panel5.panelX + panel5.panelW * 0.5F;
    const float surfaceCenter = static_cast<float>(geo.surfaceW) * 0.5F;
    TEST_CHECK(std::abs(center1 - surfaceCenter) <= 1.0F);
    TEST_CHECK(std::abs(center5 - surfaceCenter) <= 1.0F);
    TEST_CHECK(panel1.panelW < panel5.panelW);

    // Input region is confined to the capsule and not the full surface width.
    const auto region5 = shell::dock::computeInputRegion(cfg, panel5, geo.surfaceW, geo.surfaceH, false, false);
    TEST_CHECK(region5.size() == 1);
    TEST_CHECK(region5[0].width < static_cast<int>(geo.surfaceW));

    // Revealed dock with empty panel geometry falls back safely to full surface instead of returning 0x0.
    const auto fallbackRegion = shell::dock::computeInputRegion(
        cfg, shell::dock::DockPanelGeometry{}, geo.surfaceW, geo.surfaceH, /*hidden=*/false, false
    );
    TEST_CHECK(fallbackRegion.size() == 1);
    TEST_CHECK(fallbackRegion[0].x == 0);
    TEST_CHECK(fallbackRegion[0].y == 0);
    TEST_CHECK(fallbackRegion[0].width == static_cast<int>(geo.surfaceW));
    TEST_CHECK(fallbackRegion[0].height == static_cast<int>(geo.surfaceH));
  }

  // Empty dock (itemCount == 0) computes small capsule pill, not full screen span.
  {
    DockConfig cfg = flushDock(DockEdge::Bottom);
    cfg.marginEnds = 10;
    const ShellConfig::ShadowConfig shadow;
    const auto geo = shell::dock::computeSurfaceGeometry(cfg, shadow, false, 1920, 1080);
    const auto panel0 = shell::dock::computePanelGeometry(
        cfg, shadow, static_cast<float>(geo.surfaceW), static_cast<float>(geo.surfaceH), 0
    );
    const auto expectedEmptySize = static_cast<float>(shell::dock::dockContentSize(cfg, 0));
    TEST_CHECK(panel0.panelW == expectedEmptySize);
    TEST_CHECK(panel0.panelW < static_cast<float>(geo.surfaceW));
  }

  // An auto-hide dock with marginEdge > 0 extends revealed input region to screen edge to prevent Leave flicker.
  {
    DockConfig cfg = flushDock(DockEdge::Bottom);
    cfg.autoHide = true;
    cfg.marginEdge = 8;
    const ShellConfig::ShadowConfig shadow;
    const auto geo = shell::dock::computeSurfaceGeometry(cfg, shadow, false, 1920, 1080);
    const auto panel = shell::dock::computePanelGeometry(
        cfg, shadow, static_cast<float>(geo.surfaceW), static_cast<float>(geo.surfaceH), 5
    );
    const auto region =
        shell::dock::computeInputRegion(cfg, panel, geo.surfaceW, geo.surfaceH, /*hidden=*/false, false);
    TEST_CHECK(region.size() == 1);
    TEST_CHECK(region[0].y + region[0].height == static_cast<int>(geo.surfaceH));
  }

  // Empty vertical dock (itemCount == 0) computes small capsule pill, not full screen span.
  {
    DockConfig cfg = flushDock(DockEdge::Left);
    cfg.marginEnds = 10;
    const ShellConfig::ShadowConfig shadow;
    const auto geo = shell::dock::computeSurfaceGeometry(cfg, shadow, false, 1920, 1080);
    const auto panel0 = shell::dock::computePanelGeometry(
        cfg, shadow, static_cast<float>(geo.surfaceW), static_cast<float>(geo.surfaceH), 0
    );
    const auto expectedEmptySize = static_cast<float>(shell::dock::dockContentSize(cfg, 0));
    TEST_CHECK(panel0.panelH == expectedEmptySize);
    TEST_CHECK(panel0.panelH < static_cast<float>(geo.surfaceH));
  }

  // Docks with marginEdge > 0 on Top, Left, Right edges extend revealed input region to screen edge.
  for (const auto edge : {DockEdge::Top, DockEdge::Left, DockEdge::Right}) {
    DockConfig cfg = flushDock(edge);
    cfg.autoHide = true;
    cfg.marginEdge = 8;
    const ShellConfig::ShadowConfig shadow;
    const auto geo = shell::dock::computeSurfaceGeometry(cfg, shadow, false, 1920, 1080);
    const auto panel = shell::dock::computePanelGeometry(
        cfg, shadow, static_cast<float>(geo.surfaceW), static_cast<float>(geo.surfaceH), 5
    );
    const auto region =
        shell::dock::computeInputRegion(cfg, panel, geo.surfaceW, geo.surfaceH, /*hidden=*/false, false);
    TEST_CHECK(region.size() == 1);
    if (edge == DockEdge::Top) {
      TEST_CHECK(region[0].y == 0);
      TEST_CHECK(region[0].height >= static_cast<int>(panel.panelY + panel.panelH));
    } else if (edge == DockEdge::Left) {
      TEST_CHECK(region[0].x == 0);
      TEST_CHECK(region[0].width >= static_cast<int>(panel.panelX + panel.panelW));
    } else if (edge == DockEdge::Right) {
      TEST_CHECK(region[0].x + region[0].width == static_cast<int>(geo.surfaceW));
    }
  }

  for (const auto& direction : kShadowDirections) {
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
                const ShellConfig::ShadowConfig shadow{.direction = direction.value};
                const auto offset = shadowDirectionOffset(shadow.direction);
                const auto before = shell::dock::computeSurfaceGeometry(cfg, shadow, false, 1920, 1080);
                const auto beforePanel =
                    shell::dock::computePanelGeometry(cfg, shadow, before.surfaceW, before.surfaceH, 1);
                const auto beforeInput =
                    shell::dock::computeInputRegion(cfg, beforePanel, before.surfaceW, before.surfaceH, false, false);
                cfg.iconShadow = true;
                const auto surface = shell::dock::computeSurfaceGeometry(cfg, shadow, false, 1920, 1080);
                const auto panel =
                    shell::dock::computePanelGeometry(cfg, shadow, surface.surfaceW, surface.surfaceH, 1);
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
                const float left = x - growth + (offset.x - 6) * scale;
                const float right = x - growth + (cfg.iconSize + offset.x + 6) * scale;
                const float top = y - growth + (offset.y - 6) * scale;
                const float bottom = y - growth + (cfg.iconSize + offset.y + 6) * scale;
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
                // Icon shadow bleed never widens the input region along the dock or away from the screen edge.
                const auto input =
                    shell::dock::computeInputRegion(cfg, panel, surface.surfaceW, surface.surfaceH, false, false);
                TEST_CHECK(input.size() == 1);
                TEST_CHECK(beforeInput.size() == 1);
                if (vertical) {
                  TEST_CHECK(input[0].height == beforeInput[0].height);
                  TEST_CHECK(
                      std::abs(
                          (static_cast<float>(input[0].y) - panel.panelY)
                          - (static_cast<float>(beforeInput[0].y) - beforePanel.panelY)
                      )
                      <= 1.0F
                  );
                } else {
                  TEST_CHECK(input[0].width == beforeInput[0].width);
                  TEST_CHECK(
                      std::abs(
                          (static_cast<float>(input[0].x) - panel.panelX)
                          - (static_cast<float>(beforeInput[0].x) - beforePanel.panelX)
                      )
                      <= 1.0F
                  );
                }
                if (edge == DockEdge::Bottom)
                  TEST_CHECK(static_cast<float>(input[0].y) - panel.panelY == beforeInput[0].y - beforePanel.panelY);
                if (edge == DockEdge::Top)
                  TEST_CHECK(
                      static_cast<float>(input[0].y + input[0].height) - panel.panelY
                      == beforeInput[0].y + beforeInput[0].height - beforePanel.panelY
                  );
                if (edge == DockEdge::Right)
                  TEST_CHECK(static_cast<float>(input[0].x) - panel.panelX == beforeInput[0].x - beforePanel.panelX);
                if (edge == DockEdge::Left)
                  TEST_CHECK(
                      static_cast<float>(input[0].x + input[0].width) - panel.panelX
                      == beforeInput[0].x + beforeInput[0].width - beforePanel.panelX
                  );
              }
            }
          }
        }
      }
    }
  }

  return 0;
}
