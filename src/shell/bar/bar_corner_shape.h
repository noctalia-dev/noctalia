#pragma once

#include "config/config_types.h"
#include "render/core/render_styles.h"

#include <algorithm>
#include <cstdint>
#include <string_view>

/// The bar's four corners flagged for whether they sit on the bar's inner edge —
/// the edge facing away from the docked screen edge. Only inner-edge corners can
/// grow a concave notch into reserved surface space, so this is the single source
/// of truth for concave-shape mode decisions in barConcaveShape.
struct BarConcaveCorners {
  bool topLeft = false;
  bool topRight = false;
  bool bottomLeft = false;
  bool bottomRight = false;
};

/// Corners on the bar's inner edge for a given docked position. Unknown/empty
/// positions fall through to "top", matching barConcaveShape's else branch.
[[nodiscard]] inline BarConcaveCorners barInnerEdgeCorners(std::string_view position) {
  if (position == "bottom") {
    return {.topLeft = true, .topRight = true};
  }
  if (position == "left") {
    return {.topRight = true, .bottomRight = true};
  }
  if (position == "right") {
    return {.topLeft = true, .bottomLeft = true};
  }
  return {.bottomLeft = true, .bottomRight = true}; // top
}

// concave_edge_corners carves concave corners on one of the bar's two long
// edges, chosen by the margin configuration (both require margin_edge == 0):
// - Inner-edge: full-length bar (margin_ends == 0). Carves the corners on the
//   edge facing away from the screen.
// - Screen-edge: bar inset from its ends (margin_ends > 0). The bar flares
//   outward into those end margins; carves the corners touching the screen edge.
struct BarConcaveShape {
  CornerShapes corners{};
  Radii radii;
  RectInsets logicalInset{};
  float innerBulge = 0.0f; // px the surface/box grow on the inner edge
};

[[nodiscard]] inline BarConcaveShape barConcaveShape(const BarConfig& cfg) {
  const auto radius = [](std::int32_t v) { return static_cast<float>(std::max<std::int32_t>(0, v)); };
  const auto cappedRadius = [&](std::int32_t v) {
    return std::min(static_cast<float>(cfg.thickness) * 0.5f, radius(v));
  };

  BarConcaveShape g;
  g.radii = Radii{
      radius(cfg.radiusTopLeft),
      radius(cfg.radiusTopRight),
      radius(cfg.radiusBottomRight),
      radius(cfg.radiusBottomLeft),
  };

  if (!cfg.concaveEdgeCorners || cfg.marginEdge > 0) {
    return g;
  }

  const BarConcaveCorners inner = barInnerEdgeCorners(cfg.position);

  // Inner-edge concavity: full-length bar carves the corners on the inner edge.
  if (cfg.marginEnds == 0) {
    g.corners = CornerShapes{
        .tl = inner.topLeft ? CornerShape::Concave : CornerShape::Convex,
        .tr = inner.topRight ? CornerShape::Concave : CornerShape::Convex,
        .br = inner.bottomRight ? CornerShape::Concave : CornerShape::Convex,
        .bl = inner.bottomLeft ? CornerShape::Concave : CornerShape::Convex,
    };

    float bulge = 0.0f;
    if (inner.topLeft) {
      bulge = std::max(bulge, cappedRadius(cfg.radiusTopLeft));
    }
    if (inner.topRight) {
      bulge = std::max(bulge, cappedRadius(cfg.radiusTopRight));
    }
    if (inner.bottomLeft) {
      bulge = std::max(bulge, cappedRadius(cfg.radiusBottomLeft));
    }
    if (inner.bottomRight) {
      bulge = std::max(bulge, cappedRadius(cfg.radiusBottomRight));
    }
    g.innerBulge = bulge;

    const std::string_view pos = cfg.position;
    if (pos == "bottom") {
      g.logicalInset.top = g.innerBulge;
    } else if (pos == "left") {
      g.logicalInset.right = g.innerBulge;
    } else if (pos == "right") {
      g.logicalInset.left = g.innerBulge;
    } else { // top
      g.logicalInset.bottom = g.innerBulge;
    }
    return g;
  }

  // Screen-edge concavity: the bar flares outward into its end margins to meet
  // the screen edge. Each concave corner consumes its radius of that margin, so
  // scale the carve to the room available — radius, half-thickness, and
  // margin_ends — and keep radii and inset identical so the drawn arc matches
  // the reserved flare instead of overflowing it.
  const float endRoom = radius(cfg.marginEnds);
  const auto carve = [&](std::int32_t v) { return std::min(cappedRadius(v), endRoom); };
  const std::string_view pos = cfg.position;
  if (pos == "bottom") {
    g.corners.bl = CornerShape::Concave;
    g.corners.br = CornerShape::Concave;
    g.radii.bl = g.logicalInset.left = carve(cfg.radiusBottomLeft);
    g.radii.br = g.logicalInset.right = carve(cfg.radiusBottomRight);
  } else if (pos == "left") {
    g.corners.tl = CornerShape::Concave;
    g.corners.bl = CornerShape::Concave;
    g.radii.tl = g.logicalInset.top = carve(cfg.radiusTopLeft);
    g.radii.bl = g.logicalInset.bottom = carve(cfg.radiusBottomLeft);
  } else if (pos == "right") {
    g.corners.tr = CornerShape::Concave;
    g.corners.br = CornerShape::Concave;
    g.radii.tr = g.logicalInset.top = carve(cfg.radiusTopRight);
    g.radii.br = g.logicalInset.bottom = carve(cfg.radiusBottomRight);
  } else { // top
    g.corners.tl = CornerShape::Concave;
    g.corners.tr = CornerShape::Concave;
    g.radii.tl = g.logicalInset.left = carve(cfg.radiusTopLeft);
    g.radii.tr = g.logicalInset.right = carve(cfg.radiusTopRight);
  }

  return g;
}
