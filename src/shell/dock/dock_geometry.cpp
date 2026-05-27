#include "shell/dock/dock_geometry.h"

#include "shell/surface_shadow.h"
#include "wayland/layer_surface.h"

#include <algorithm>
#include <cmath>

namespace shell::dock {
  namespace {

    constexpr std::int32_t kCellPad = 6;
    constexpr std::int32_t kAutoHideTriggerPx = 2;
    constexpr float kAutoHideSlideExtraPx = 16.0f;

  } // namespace

  std::uint32_t positionToAnchor(const std::string& position) {
    if (position == "top") {
      return LayerShellAnchor::Top;
    }
    if (position == "left") {
      return LayerShellAnchor::Left;
    }
    if (position == "right") {
      return LayerShellAnchor::Right;
    }
    return LayerShellAnchor::Bottom;
  }

  bool isVerticalPosition(const std::string& position) { return position == "left" || position == "right"; }

  std::int32_t dockContentSize(const DockConfig& cfg, std::size_t itemCount) {
    const auto n = static_cast<std::int32_t>(itemCount);
    const std::int32_t cellSize = cfg.iconSize + (kCellPad * 2);
    if (n == 0) {
      return cellSize + (cfg.padding * 2);
    }
    return (n * cellSize) + (std::max(0, n - 1) * cfg.itemSpacing) + (cfg.padding * 2);
  }

  std::int32_t dockThickness(const DockConfig& cfg) { return cfg.iconSize + (kCellPad * 2) + (cfg.padding * 2); }

  std::size_t dockLauncherButtonCount(const DockConfig& cfg) {
    return (cfg.launcherPosition == "start" || cfg.launcherPosition == "end") ? 1U : 0U;
  }

  DockSurfaceGeometry
  computeSurfaceGeometry(const DockConfig& cfg, const ShellConfig::ShadowConfig& shadow, std::size_t itemCount) {
    const bool vertical = isVerticalPosition(cfg.position);
    const auto sb = shell::surface_shadow::bleed(cfg.shadow, shadow);
    const bool hiddenOverlayMode = cfg.autoHide && !cfg.reserveSpace;
    const auto panelW = dockContentSize(cfg, itemCount);
    const auto panelH = dockThickness(cfg);
    const bool isBottom = (cfg.position == "bottom");
    const bool isRight = (cfg.position == "right");

    DockSurfaceGeometry geometry;
    if (!vertical) {
      geometry.surfaceW = static_cast<std::uint32_t>(panelW + sb.left + sb.right);
      geometry.surfaceH = static_cast<std::uint32_t>(sb.up + panelH + std::min(cfg.marginEdge, sb.down));
      if (isBottom) {
        geometry.marginBottom = std::max(0, cfg.marginEdge - sb.down);
        geometry.surfaceH = static_cast<std::uint32_t>(sb.up + panelH + std::min(cfg.marginEdge, sb.down));
        geometry.exclusiveZone = hiddenOverlayMode ? 0 : (panelH + std::min(cfg.marginEdge, sb.down));
      } else {
        geometry.marginTop = std::max(0, cfg.marginEdge - sb.up);
        geometry.surfaceH = static_cast<std::uint32_t>(std::min(cfg.marginEdge, sb.up) + panelH + sb.down);
        geometry.exclusiveZone = hiddenOverlayMode ? 0 : (std::min(cfg.marginEdge, sb.up) + panelH);
      }
      geometry.marginLeft = cfg.marginEnds;
      geometry.marginRight = cfg.marginEnds;
      return geometry;
    }

    geometry.surfaceH = static_cast<std::uint32_t>(panelW + sb.up + sb.down);
    if (isRight) {
      geometry.marginRight = std::max(0, cfg.marginEdge - sb.right);
      geometry.surfaceW = static_cast<std::uint32_t>(sb.left + panelH + std::min(cfg.marginEdge, sb.right));
      geometry.exclusiveZone = hiddenOverlayMode ? 0 : (panelH + std::min(cfg.marginEdge, sb.right));
    } else {
      geometry.marginLeft = std::max(0, cfg.marginEdge - sb.left);
      geometry.surfaceW = static_cast<std::uint32_t>(std::min(cfg.marginEdge, sb.left) + panelH + sb.right);
      geometry.exclusiveZone = hiddenOverlayMode ? 0 : (std::min(cfg.marginEdge, sb.left) + panelH);
    }
    geometry.marginTop = cfg.marginEnds;
    geometry.marginBottom = cfg.marginEnds;
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
    const bool vertical = isVerticalPosition(cfg.position);
    const auto sb = shell::surface_shadow::bleed(cfg.shadow, shadow);
    const float bleedL = static_cast<float>(sb.left);
    const float bleedR = static_cast<float>(sb.right);
    const float bleedU = static_cast<float>(sb.up);
    const float bleedD = static_cast<float>(sb.down);
    const float mEdge = static_cast<float>(cfg.marginEdge);
    const bool isBottom = (cfg.position == "bottom");
    const bool isRight = (cfg.position == "right");

    if (!vertical) {
      return DockPanelGeometry{
          .panelX = bleedL,
          .panelY = isBottom ? bleedU : std::min(mEdge, bleedU),
          .panelW = surfaceW - bleedL - bleedR,
          .panelH = static_cast<float>(dockThickness(cfg)),
      };
    }

    return DockPanelGeometry{
        .panelX = isRight ? bleedL : std::min(mEdge, bleedL),
        .panelY = bleedU,
        .panelW = static_cast<float>(dockThickness(cfg)),
        .panelH = surfaceH - bleedU - bleedD,
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
      const float sx = panel.panelX + static_cast<float>(shadow.offsetX);
      const float sy = panel.panelY + static_cast<float>(shadow.offsetY);
      contentLeft = std::min(contentLeft, sx);
      contentTop = std::min(contentTop, sy);
      contentRight = std::max(contentRight, sx + panel.panelW);
      contentBottom = std::max(contentBottom, sy + panel.panelH);
    }

    if (!isVerticalPosition(cfg.position)) {
      if (cfg.position == "bottom") {
        return {0.0f, (surfaceH - contentTop) + kAutoHideSlideExtraPx};
      }
      return {0.0f, -(contentBottom + kAutoHideSlideExtraPx)};
    }
    if (cfg.position == "right") {
      return {(surfaceW - contentLeft) + kAutoHideSlideExtraPx, 0.0f};
    }
    return {-(contentRight + kAutoHideSlideExtraPx), 0.0f};
  }

  std::vector<InputRect>
  computeInputRegion(const DockConfig& cfg, const DockPanelGeometry& panel, int surfaceW, int surfaceH, bool hidden) {
    if (hidden) {
      if (!isVerticalPosition(cfg.position)) {
        return {InputRect{0, surfaceH - kAutoHideTriggerPx, surfaceW, kAutoHideTriggerPx}};
      }
      if (cfg.position == "left") {
        return {InputRect{surfaceW - kAutoHideTriggerPx, 0, kAutoHideTriggerPx, surfaceH}};
      }
      return {InputRect{0, 0, kAutoHideTriggerPx, surfaceH}};
    }

    return {InputRect{
        static_cast<int>(std::lround(panel.panelX)),
        static_cast<int>(std::lround(panel.panelY)),
        static_cast<int>(std::lround(panel.panelW)),
        static_cast<int>(std::lround(panel.panelH)),
    }};
  }

} // namespace shell::dock
