#pragma once

#include "render/core/renderer.h"

#include <string>
#include <string_view>
#include <variant>
#include <vector>

struct TooltipRow {
  std::string key;
  std::string value;
  TextEllipsize valueEllipsize = TextEllipsize::End;
};

enum class TooltipPlacement : std::uint8_t {
  Default,
  Above,
  Below,
  Left,
  Right,
};

// Place tooltips on the outward side of a screen-edge surface (bar/dock).
// `edge` is "top" | "bottom" | "left" | "right"; unknown values open below.
[[nodiscard]] inline TooltipPlacement tooltipPlacementAwayFromEdge(std::string_view edge) noexcept {
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

struct TooltipAnchorInsets {
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;
  float left = 0.0f;
};

using TooltipContent = std::variant<std::monostate, std::string, std::vector<TooltipRow>>;
