#pragma once

#include "wayland/layer_surface.h"

#include <cstdint>
#include <string_view>

// Maps a screen-position token (the `<panel>_position` / OSD `position` vocabulary:
// top_left, top_center, top_right, center_left, center_right, bottom_left,
// bottom_center, bottom_right) to layer-shell anchor edges plus a per-edge margin.
// Anchoring one edge centers the surface on the perpendicular axis; anchoring a
// corner pins it there. "center" (dead-centre) and "auto" (bar-relative) are
// handled by the caller, not here.
namespace shell {

  struct ScreenPositionAnchor {
    std::uint32_t anchor = 0;
    std::int32_t marginTop = 0;
    std::int32_t marginRight = 0;
    std::int32_t marginBottom = 0;
    std::int32_t marginLeft = 0;
  };

  [[nodiscard]] inline ScreenPositionAnchor screenPositionAnchor(std::string_view position, std::int32_t edge) {
    const bool top = position == "top_left" || position == "top_center" || position == "top_right";
    const bool bottom = position == "bottom_left" || position == "bottom_center" || position == "bottom_right";
    const bool left = position == "top_left" || position == "center_left" || position == "bottom_left";
    const bool right = position == "top_right" || position == "center_right" || position == "bottom_right";

    ScreenPositionAnchor out;
    if (top) {
      out.anchor |= LayerShellAnchor::Top;
      out.marginTop = edge;
    }
    if (bottom) {
      out.anchor |= LayerShellAnchor::Bottom;
      out.marginBottom = edge;
    }
    if (left) {
      out.anchor |= LayerShellAnchor::Left;
      out.marginLeft = edge;
    }
    if (right) {
      out.anchor |= LayerShellAnchor::Right;
      out.marginRight = edge;
    }
    return out;
  }

  // Resolves the bar-relative start/end alignment an *attached* panel should take
  // from the same `<panel>_position` vocabulary, restricted to the component that
  // matches the bar's own axis (left/right for a horizontal bar, top/bottom for a
  // vertical bar). The perpendicular component and "auto"/"center" carry no
  // alignment preference; the caller keeps its existing bar-centered default then.
  struct AttachedPositionAlign {
    bool start = false;
    bool end = false;
  };

  [[nodiscard]] inline AttachedPositionAlign attachedPositionAlign(std::string_view position, bool barIsVertical) {
    if (barIsVertical) {
      return {
          .start = position == "top_left" || position == "top_center" || position == "top_right",
          .end = position == "bottom_left" || position == "bottom_center" || position == "bottom_right",
      };
    }
    return {
        .start = position == "top_left" || position == "center_left" || position == "bottom_left",
        .end = position == "top_right" || position == "center_right" || position == "bottom_right",
    };
  }

} // namespace shell
