#pragma once

#include <string_view>

/// The bar's four corners flagged for whether they sit on the bar's inner edge —
/// the edge facing away from the docked screen edge. Only inner-edge corners can
/// grow a concave notch into reserved surface space, so this is the single source
/// of truth for both the renderer (barConcaveShape) and the settings UI (which
/// corners may be inverted).
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
