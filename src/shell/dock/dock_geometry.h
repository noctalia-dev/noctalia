#pragma once

#include "config/config_types.h"
#include "render/core/render_styles.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

struct InputRect;
struct LayerSurfaceConfig;

namespace shell::dock {

  struct DockPanelGeometry {
    float panelX = 0.0F;
    float panelY = 0.0F;
    float panelW = 0.0F;
    float panelH = 0.0F;
  };

  struct DockSurfaceGeometry {
    std::uint32_t surfaceW = 0;
    std::uint32_t surfaceH = 0;
    std::int32_t marginTop = 0;
    std::int32_t marginRight = 0;
    std::int32_t marginBottom = 0;
    std::int32_t marginLeft = 0;
    std::int32_t exclusiveZone = 0;
  };

  struct DockConcaveShape {
    CornerShapes corners{};
    Radii radii;
    RectInsets logicalInset{};
  };

  DockConcaveShape dockConcaveShape(const DockConfig& cfg);

  std::uint32_t positionToAnchor(DockEdge edge);
  bool isVerticalEdge(DockEdge edge);
  void shiftAlongEdge(DockEdge edge, float& x, float& y, float amount);
  std::int32_t dockContentSize(const DockConfig& cfg, std::size_t itemCount);
  std::int32_t dockThickness(const DockConfig& cfg);
  // Extra cross-axis surface padding so magnified icons (and badges) are not clipped.
  std::int32_t dockHoverZoomCrossPad(const DockConfig& cfg);
  // Extra main-axis end padding for magnified icon/badge overhang past the pill ends.
  std::int32_t dockHoverZoomMainPad(const DockConfig& cfg);
  std::size_t dockLauncherButtonCount(DockLauncherPosition position);
  std::size_t dockLauncherButtonCount(const DockConfig& cfg);
  // The surface spans the output's main axis minus `margin_ends` regardless of item count, so
  // window changes never resize the layer surface; computePanelGeometry centers the capsule in it.
  // `fractionalScale` marks an output whose scale is not an integer: a flush dock overlaps the
  // screen edge by a logical pixel there, because the compositor can otherwise round the edge a
  // device pixel short and leave a gap.
  DockSurfaceGeometry computeSurfaceGeometry(
      const DockConfig& cfg, const ShellConfig::ShadowConfig& shadow, bool fractionalScale,
      std::int32_t outputLogicalWidth, std::int32_t outputLogicalHeight
  );
  LayerSurfaceConfig makeLayerSurfaceConfig(
      const DockConfig& cfg, const ShellConfig::ShadowConfig& shadow, bool fractionalScale,
      std::int32_t outputLogicalWidth, std::int32_t outputLogicalHeight
  );
  DockPanelGeometry computePanelGeometry(
      const DockConfig& cfg, const ShellConfig::ShadowConfig& shadow, float surfaceW, float surfaceH,
      std::size_t itemCount
  );
  std::pair<float, float> computeHiddenSlideDelta(
      const DockConfig& cfg, const ShellConfig::ShadowConfig& shadow, float surfaceW, float surfaceH,
      const DockPanelGeometry& panel
  );
  std::vector<InputRect> computeInputRegion(
      const DockConfig& cfg, const DockPanelGeometry& panel, int surfaceW, int surfaceH, bool hidden,
      bool fractionalScale
  );

} // namespace shell::dock
