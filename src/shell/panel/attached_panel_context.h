#pragma once

#include "render/core/render_styles.h"

#include <cstdint>
#include <string_view>

enum class AttachedRevealDirection : std::uint8_t {
  Down,
  Up,
  Right,
  Left,
};

// Which end of the bar's own axis an attached panel is pinned to via its
// `*_position` alignment (left/right for a horizontal bar, top/bottom for a
// vertical one). None covers the bar-centered and click-anchored cases.
enum class AttachedEdgeAlign : std::uint8_t {
  None,
  Start,
  End,
};

struct AttachedPanelGeometry {
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
  // Full panel corner radius; used for the away-side convex corners, which are visible
  // throughout the open/close animation and should always render rounded.
  float cornerRadius = 0.0F;
  // Bar-side concave-corner radius. Animated with the reveal progress: zero while the
  // bg's bar-side edge is still hidden behind the bar, ramps to cornerRadius as the
  // bulges slide into view near the end of the open animation.
  float bulgeRadius = 0.0F;
};

namespace attached_panel {

  [[nodiscard]] inline CornerShapes cornerShapes(std::string_view barPosition) {
    if (barPosition == "bottom") {
      return CornerShapes{
          .tl = CornerShape::Convex,
          .tr = CornerShape::Convex,
          .br = CornerShape::Concave,
          .bl = CornerShape::Concave,
      };
    }
    if (barPosition == "left") {
      return CornerShapes{
          .tl = CornerShape::Concave,
          .tr = CornerShape::Convex,
          .br = CornerShape::Convex,
          .bl = CornerShape::Concave,
      };
    }
    if (barPosition == "right") {
      return CornerShapes{
          .tl = CornerShape::Convex,
          .tr = CornerShape::Concave,
          .br = CornerShape::Concave,
          .bl = CornerShape::Convex,
      };
    }
    // top (default)
    return CornerShapes{
        .tl = CornerShape::Concave,
        .tr = CornerShape::Concave,
        .br = CornerShape::Convex,
        .bl = CornerShape::Convex,
    };
  }

  // The away-side corner radius goes square (0) at whichever end of the bar's own
  // axis the panel is edge-aligned to via `*_position` — it butts flush against the
  // screen edge there instead of rounding into open space. Only that one away-side
  // corner changes; the bar-side corner on the same end keeps its normal concave
  // nesting into the bar, untouched.
  [[nodiscard]] inline Radii edgeAlignedRadii(std::string_view barPosition, AttachedEdgeAlign alignEdge, float radius) {
    Radii radii{radius, radius, radius, radius};
    if (alignEdge == AttachedEdgeAlign::None) {
      return radii;
    }
    if (barPosition == "bottom") {
      if (alignEdge == AttachedEdgeAlign::Start) {
        radii.tl = 0.0F;
      } else {
        radii.tr = 0.0F;
      }
    } else if (barPosition == "left") {
      if (alignEdge == AttachedEdgeAlign::Start) {
        radii.tr = 0.0F;
      } else {
        radii.br = 0.0F;
      }
    } else if (barPosition == "right") {
      if (alignEdge == AttachedEdgeAlign::Start) {
        radii.tl = 0.0F;
      } else {
        radii.bl = 0.0F;
      }
    } else {
      // top (default)
      if (alignEdge == AttachedEdgeAlign::Start) {
        radii.bl = 0.0F;
      } else {
        radii.br = 0.0F;
      }
    }
    return radii;
  }

  [[nodiscard]] inline RectInsets logicalInset(std::string_view barPosition, float radius) {
    const bool vertical = (barPosition == "left" || barPosition == "right");
    if (vertical) {
      return RectInsets{
          .left = 0.0F,
          .top = radius,
          .right = 0.0F,
          .bottom = radius,
      };
    }
    return RectInsets{
        .left = radius,
        .top = 0.0F,
        .right = radius,
        .bottom = 0.0F,
    };
  }

  [[nodiscard]] inline AttachedRevealDirection revealDirection(std::string_view barPosition) {
    if (barPosition == "bottom") {
      return AttachedRevealDirection::Up;
    }
    if (barPosition == "left") {
      return AttachedRevealDirection::Right;
    }
    if (barPosition == "right") {
      return AttachedRevealDirection::Left;
    }
    return AttachedRevealDirection::Down;
  }

  [[nodiscard]] inline Radii cornerRadii(std::string_view barPosition, float radius) {
    if (barPosition == "bottom") {
      return Radii{0.0F, 0.0F, radius, radius};
    }
    if (barPosition == "left") {
      return Radii{0.0F, radius, radius, 0.0F};
    }
    if (barPosition == "right") {
      return Radii{radius, 0.0F, 0.0F, radius};
    }
    return Radii{radius, radius, 0.0F, 0.0F};
  }

} // namespace attached_panel
