#pragma once

#include "render/scene/tooltip_content.h"

#include <string_view>

// Place tooltips on the outward side of a screen-edge surface (bar/dock).
// `edge` is "top" | "bottom" | "left" | "right"; unknown values open below.
inline TooltipPlacement tooltipPlacementAwayFromEdge(std::string_view edge) noexcept {
  if (edge == "bottom") {
    return TooltipPlacement::Above;
  }
  if (edge == "left") {
    return TooltipPlacement::Right;
  }
  if (edge == "right") {
    return TooltipPlacement::Left;
  }
  return TooltipPlacement::Below;
}
