#include "shell/dock/dock_geometry.h"

#include "shell/surface/shadow.h"
#include "wayland/layer_surface.h"

#include <algorithm>
#include <cmath>

namespace shell::dock {
  namespace {

    constexpr std::int32_t kCellPad = 6;
    constexpr std::int32_t kAutoHideTriggerPx = 2;
    constexpr float kAutoHideSlideExtraPx = 16.0f;

    [[nodiscard]] int dockAutoHideEdgeGutter(const DockConfig& cfg) noexcept {
      if (!cfg.autoHide || cfg.marginEdge <= 0) {
        return 0;
      }
      return cfg.marginEdge;
    }

  } // namespace

  DockConcaveShape dockConcaveShape(const DockConfig& cfg) {
    DockConcaveShape g;
    g.radii = Radii{
        static_cast<float>(cfg.radiusTopLeft),
        static_cast<float>(cfg.radiusTopRight),
        static_cast<float>(cfg.radiusBottomRight),
        static_cast<float>(cfg.radiusBottomLeft),
    };

    if (!cfg.concaveEdgeCorners || cfg.marginEdge > 0) {
      return g;
    }

    // Ceil so logicalInset is integral: surface sizing, panel placement, and blur
    // tessellation all consume it and must agree on whole pixels.
    const float cap = static_cast<float>(dockThickness(cfg)) * 0.5f;
    const auto capped = [&](float v) { return std::ceil(std::min(cap, v)); };

    // Carve concavity only on corners facing the screen edge.
    switch (cfg.position) {
    case DockEdge::Bottom:
      g.corners.bl = CornerShape::Concave;
      g.corners.br = CornerShape::Concave;
      g.logicalInset.left = capped(g.radii.bl);
      g.logicalInset.right = capped(g.radii.br);
      break;
    case DockEdge::Top:
      g.corners.tl = CornerShape::Concave;
      g.corners.tr = CornerShape::Concave;
      g.logicalInset.left = capped(g.radii.tl);
      g.logicalInset.right = capped(g.radii.tr);
      break;
    case DockEdge::Left:
      g.corners.tl = CornerShape::Concave;
      g.corners.bl = CornerShape::Concave;
      g.logicalInset.top = capped(g.radii.tl);
      g.logicalInset.bottom = capped(g.radii.bl);
      break;
    case DockEdge::Right:
      g.corners.tr = CornerShape::Concave;
      g.corners.br = CornerShape::Concave;
      g.logicalInset.top = capped(g.radii.tr);
      g.logicalInset.bottom = capped(g.radii.br);
      break;
    }
    return g;
  }

  std::uint32_t positionToAnchor(DockEdge edge) {
    if (edge == DockEdge::Top) {
      return LayerShellAnchor::Top;
    }
    if (edge == DockEdge::Left) {
      return LayerShellAnchor::Left;
    }
    if (edge == DockEdge::Right) {
      return LayerShellAnchor::Right;
    }
    return LayerShellAnchor::Bottom;
  }

  bool isVerticalEdge(DockEdge edge) { return edge == DockEdge::Left || edge == DockEdge::Right; }

  void shiftAlongEdge(DockEdge edge, float& x, float& y, float amount) {
    switch (edge) {
    case DockEdge::Bottom:
      y += amount;
      break;
    case DockEdge::Top:
      y -= amount;
      break;
    case DockEdge::Left:
      x -= amount;
      break;
    case DockEdge::Right:
      x += amount;
      break;
    }
  }

  std::int32_t dockContentSize(const DockConfig& cfg, std::size_t itemCount) {
    const auto n = static_cast<std::int32_t>(itemCount);
    const std::int32_t cellSize = cfg.iconSize + kCellPad * 2;
    if (n == 0) {
      return cellSize + cfg.mainAxisPadding * 2;
    }
    return n * cellSize + std::max(0, n - 1) * cfg.itemSpacing + cfg.mainAxisPadding * 2;
  }

  std::int32_t dockThickness(const DockConfig& cfg) { return cfg.iconSize + kCellPad * 2 + cfg.crossAxisPadding * 2; }

  std::int32_t dockHoverZoomCrossPad(const DockConfig& cfg) {
    if (!cfg.magnification || cfg.magnificationScale <= 1.0f) {
      return 0;
    }
    const float extra = static_cast<float>(cfg.iconSize) * (cfg.magnificationScale - 1.0f) * 0.5f;
    return static_cast<std::int32_t>(std::ceil(extra * 1.35f + static_cast<float>(kCellPad)));
  }

  std::size_t dockLauncherButtonCount(DockLauncherPosition position) {
    return position == DockLauncherPosition::Start || position == DockLauncherPosition::End ? 1U : 0U;
  }

  std::size_t dockLauncherButtonCount(const DockConfig& cfg) { return dockLauncherButtonCount(cfg.launcherPosition); }

  DockSurfaceGeometry
  computeSurfaceGeometry(const DockConfig& cfg, const ShellConfig::ShadowConfig& shadow, std::size_t itemCount) {
    const DockEdge edge = cfg.position;
    const bool vertical = isVerticalEdge(edge);
    const auto sb = shell::surface_shadow::bleed(cfg.shadow, shadow);
    const auto concave = dockConcaveShape(cfg);
    const int insetL = static_cast<int>(concave.logicalInset.left);
    const int insetT = static_cast<int>(concave.logicalInset.top);
    const int insetR = static_cast<int>(concave.logicalInset.right);
    const int insetB = static_cast<int>(concave.logicalInset.bottom);
    const auto panelW = dockContentSize(cfg, itemCount);
    const auto panelH = dockThickness(cfg);
    const std::int32_t zoomPad = dockHoverZoomCrossPad(cfg);
    const bool isBottom = edge == DockEdge::Bottom;
    const bool isRight = edge == DockEdge::Right;
    const std::int32_t mEdge = cfg.marginEdge;
    const int edgeGutter = dockAutoHideEdgeGutter(cfg);

    DockSurfaceGeometry geometry;
    if (!vertical) {
      geometry.surfaceW = static_cast<std::uint32_t>(panelW + sb.left + sb.right + insetL + insetR);
      geometry.marginLeft = cfg.marginEnds;
      geometry.marginRight = cfg.marginEnds;
      if (isBottom) {
        if (edgeGutter > 0) {
          geometry.surfaceH = static_cast<std::uint32_t>(sb.up + panelH + edgeGutter + zoomPad);
        } else {
          geometry.marginBottom = std::max(0, mEdge - sb.down);
          geometry.surfaceH = static_cast<std::uint32_t>(sb.up + panelH + std::min(mEdge, sb.down) + zoomPad);
        }
        geometry.exclusiveZone = cfg.reserveSpace ? (panelH + std::min(mEdge, sb.down)) : 0;
      } else {
        if (edgeGutter > 0) {
          geometry.surfaceH = static_cast<std::uint32_t>(sb.down + panelH + edgeGutter + zoomPad);
        } else {
          geometry.marginTop = std::max(0, mEdge - sb.up);
          geometry.surfaceH = static_cast<std::uint32_t>(std::min(mEdge, sb.up) + panelH + sb.down + zoomPad);
        }
        geometry.exclusiveZone = cfg.reserveSpace ? (std::min(mEdge, sb.up) + panelH) : 0;
      }
      return geometry;
    }

    geometry.marginTop = cfg.marginEnds;
    geometry.marginBottom = cfg.marginEnds;
    geometry.surfaceH = static_cast<std::uint32_t>(panelW + sb.up + sb.down + insetT + insetB);
    if (isRight) {
      if (edgeGutter > 0) {
        geometry.surfaceW = static_cast<std::uint32_t>(sb.left + panelH + edgeGutter + zoomPad);
      } else {
        geometry.marginRight = std::max(0, mEdge - sb.right);
        geometry.surfaceW = static_cast<std::uint32_t>(sb.left + panelH + std::min(mEdge, sb.right) + zoomPad);
      }
      geometry.exclusiveZone = cfg.reserveSpace ? (panelH + std::min(mEdge, sb.right)) : 0;
    } else {
      if (edgeGutter > 0) {
        geometry.surfaceW = static_cast<std::uint32_t>(sb.right + panelH + edgeGutter + zoomPad);
      } else {
        geometry.marginLeft = std::max(0, mEdge - sb.left);
        geometry.surfaceW = static_cast<std::uint32_t>(std::min(mEdge, sb.left) + panelH + sb.right + zoomPad);
      }
      geometry.exclusiveZone = cfg.reserveSpace ? (std::min(mEdge, sb.left) + panelH) : 0;
    }
    return geometry;
  }

  LayerSurfaceConfig
  makeLayerSurfaceConfig(const DockConfig& cfg, const ShellConfig::ShadowConfig& shadow, std::size_t itemCount) {
    const auto geometry = computeSurfaceGeometry(cfg, shadow, itemCount);
    return LayerSurfaceConfig{
        .nameSpace = "noctalia-dock",
        .layer = LayerShellLayer::Top,
        .anchor = positionToAnchor(cfg.position),
        .width = geometry.surfaceW,
        .height = geometry.surfaceH,
        .exclusiveZone = geometry.exclusiveZone,
        .marginTop = geometry.marginTop,
        .marginRight = geometry.marginRight,
        .marginBottom = geometry.marginBottom,
        .marginLeft = geometry.marginLeft,
        .defaultWidth = geometry.surfaceW,
        .defaultHeight = geometry.surfaceH,
    };
  }

  DockPanelGeometry
  computePanelGeometry(const DockConfig& cfg, const ShellConfig::ShadowConfig& shadow, float surfaceW, float surfaceH) {
    const DockEdge edge = cfg.position;
    const bool vertical = isVerticalEdge(edge);
    const auto sb = shell::surface_shadow::bleed(cfg.shadow, shadow);
    const auto concave = dockConcaveShape(cfg);
    const float insetL = concave.logicalInset.left;
    const float insetT = concave.logicalInset.top;
    const float insetR = concave.logicalInset.right;
    const float insetB = concave.logicalInset.bottom;
    const auto bleedL = static_cast<float>(sb.left);
    const auto bleedR = static_cast<float>(sb.right);
    const auto bleedU = static_cast<float>(sb.up);
    const auto bleedD = static_cast<float>(sb.down);
    const auto mEdge = static_cast<float>(cfg.marginEdge);
    const bool isBottom = edge == DockEdge::Bottom;
    const bool isRight = edge == DockEdge::Right;
    const auto panelThickness = static_cast<float>(dockThickness(cfg));

    if (!vertical) {
      float y = isBottom ? surfaceH - std::min(mEdge, bleedD) - panelThickness : std::min(mEdge, bleedU);
      if (const int gutter = dockAutoHideEdgeGutter(cfg); gutter > 0) {
        if (isBottom) {
          y = surfaceH - static_cast<float>(gutter) - panelThickness;
        } else {
          y = static_cast<float>(gutter);
        }
      }
      return DockPanelGeometry{
          .panelX = bleedL + insetL,
          .panelY = y,
          .panelW = surfaceW - bleedL - bleedR - insetL - insetR,
          .panelH = panelThickness,
      };
    }

    float x = isRight ? surfaceW - std::min(mEdge, bleedR) - panelThickness : std::min(mEdge, bleedL);
    if (const int gutter = dockAutoHideEdgeGutter(cfg); gutter > 0) {
      if (isRight) {
        x = surfaceW - static_cast<float>(gutter) - panelThickness;
      } else {
        x = static_cast<float>(gutter);
      }
    }
    return DockPanelGeometry{
        .panelX = x,
        .panelY = bleedU + insetT,
        .panelW = panelThickness,
        .panelH = surfaceH - bleedU - bleedD - insetT - insetB,
    };
  }

  std::pair<float, float> computeHiddenSlideDelta(
      const DockConfig& cfg, const ShellConfig::ShadowConfig& shadow, float surfaceW, float surfaceH,
      const DockPanelGeometry& panel
  ) {
    float contentLeft = panel.panelX;
    float contentTop = panel.panelY;
    float contentRight = panel.panelX + panel.panelW;
    float contentBottom = panel.panelY + panel.panelH;
    if (shell::surface_shadow::enabled(cfg.shadow, shadow)) {
      const auto offset = shadowDirectionOffset(shadow.direction);
      const float sx = panel.panelX + static_cast<float>(offset.x);
      const float sy = panel.panelY + static_cast<float>(offset.y);
      contentLeft = std::min(contentLeft, sx);
      contentTop = std::min(contentTop, sy);
      contentRight = std::max(contentRight, sx + panel.panelW);
      contentBottom = std::max(contentBottom, sy + panel.panelH);
    }

    const DockEdge edge = cfg.position;
    if (!isVerticalEdge(edge)) {
      if (edge == DockEdge::Bottom) {
        return {0.0f, (surfaceH - contentTop) + kAutoHideSlideExtraPx};
      }
      return {0.0f, -(contentBottom + kAutoHideSlideExtraPx)};
    }
    if (edge == DockEdge::Right) {
      return {(surfaceW - contentLeft) + kAutoHideSlideExtraPx, 0.0f};
    }
    return {-(contentRight + kAutoHideSlideExtraPx), 0.0f};
  }

  std::vector<InputRect>
  computeInputRegion(const DockConfig& cfg, const DockPanelGeometry& panel, int surfaceW, int surfaceH, bool hidden) {
    if (hidden) {
      const DockEdge edge = cfg.position;
      if (edge == DockEdge::Bottom) {
        return {InputRect{0, surfaceH - kAutoHideTriggerPx, surfaceW, kAutoHideTriggerPx}};
      }
      if (edge == DockEdge::Left) {
        return {InputRect{0, 0, kAutoHideTriggerPx, surfaceH}};
      }
      if (edge == DockEdge::Right) {
        return {InputRect{surfaceW - kAutoHideTriggerPx, 0, kAutoHideTriggerPx, surfaceH}};
      }
      return {InputRect{0, 0, surfaceW, kAutoHideTriggerPx}};
    }

    return {InputRect{
        static_cast<int>(std::lround(panel.panelX)),
        static_cast<int>(std::lround(panel.panelY)),
        static_cast<int>(std::lround(panel.panelW)),
        static_cast<int>(std::lround(panel.panelH)),
    }};
  }

} // namespace shell::dock
