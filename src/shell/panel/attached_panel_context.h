#pragma once

#include "render/core/render_styles.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

enum class AttachedRevealDirection : std::uint8_t {
  Down,
  Up,
  Right,
  Left,
};

struct AttachedPanelGeometry {
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
  float cornerRadius = 0.0F;
  // The concave shoulders grow from the bar as the panel opens.
  float bulgeRadius = 0.0F;
};

namespace attached_panel {

  inline CornerShapes cornerShapes(std::string_view barPosition) {
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

  inline RectInsets logicalInset(std::string_view barPosition, float radius) {
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

  inline AttachedRevealDirection revealDirection(std::string_view barPosition) {
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

  inline Radii cornerRadii(const CornerShapes& corners, float bulge, float away) {
    const auto radius = [&](CornerShape shape) { return shape == CornerShape::Concave ? bulge : away; };
    return {radius(corners.tl), radius(corners.tr), radius(corners.br), radius(corners.bl)};
  }

  // Input is the full body, excluding shoulder insets. Return the visible
  // decoration bounds in the same coordinate space, including those insets.
  inline AttachedPanelGeometry revealGeometry(AttachedPanelGeometry body, std::string_view edge, float progress) {
    const bool verticalBar = edge == "left" || edge == "right";
    const float depth = verticalBar ? body.width : body.height;
    const float visible = std::round(depth * progress);
    // Ease the shoulders out as travel begins, then settle into the open radius.
    const float shoulderProgress = progress * (2.0F - progress);
    const float bulge = std::min(body.cornerRadius * shoulderProgress, visible * 0.5F);
    body.bulgeRadius = bulge;
    if (verticalBar) {
      if (edge == "right") {
        body.x += depth - visible;
      }
      body.width = visible;
      body.y -= bulge;
      body.height += 2.0F * bulge;
    } else {
      if (edge == "bottom") {
        body.y += depth - visible;
      }
      body.height = visible;
      body.x -= bulge;
      body.width += 2.0F * bulge;
    }
    return body;
  }

} // namespace attached_panel
