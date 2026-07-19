#pragma once

// Pure geometry for the screen frame, split out so it can be tested without a compositor,
// a renderer or a live bar. Frame::buildFrameScene is a thin translation of these results
// into scene nodes; everything that decides *where* the frame paints lives here.

#include "config/config_types.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace frame_geometry {

  enum class Edge : int { None = -1, Top = 0, Bottom = 1, Left = 2, Right = 3 };

  struct Insets {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    bool operator==(const Insets&) const = default;
  };

  struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    [[nodiscard]] bool empty() const noexcept { return width <= 0.0f || height <= 0.0f; }
    bool operator==(const Rect&) const = default;
  };

  [[nodiscard]] inline Edge edgeForPosition(std::string_view position) noexcept {
    if (position == "bottom") {
      return Edge::Bottom;
    }
    if (position == "left") {
      return Edge::Left;
    }
    if (position == "right") {
      return Edge::Right;
    }
    return Edge::Top;
  }

  // The bar the frame defers to, for fill and for its edge alike. Deliberately independent of
  // `reserveSpace`: a bar occupies its edge whether or not it displaces windows.
  [[nodiscard]] inline const BarConfig* styleBar(const Config& config) noexcept {
    for (const auto& bar : config.bars) {
      if (bar.enabled) {
        return &bar;
      }
    }
    return nullptr;
  }

  // Reserved insets of the hole from each output edge. The bar's edge takes the bar's own
  // thickness — never thickness + frame thickness — but only while the band is still the
  // bar's. Once the bar has gone AND released the band, windows own it and the frame falls
  // back to its own thickness so the border still closes.
  [[nodiscard]] inline Insets
  holeInsets(const ShellConfig::FrameConfig& cfg, const BarConfig* bar, bool barVisible, bool barReserves) noexcept {
    const auto thickness = static_cast<float>(std::max(0, cfg.thickness));
    Insets insets{thickness, thickness, thickness, thickness};
    if (bar == nullptr || (!barVisible && !barReserves)) {
      return insets;
    }
    const auto barThickness = static_cast<float>(std::max(0, bar->thickness));
    switch (edgeForPosition(bar->position)) {
    case Edge::Bottom:
      insets.bottom = barThickness;
      break;
    case Edge::Left:
      insets.left = barThickness;
      break;
    case Edge::Right:
      insets.right = barThickness;
      break;
    default:
      insets.top = barThickness;
      break;
    }
    return insets;
  }

  // Opacity of the frame fill. Matching the bar does NOT depend on reservation: a bar that
  // reserves nothing still sits on screen and still has to match.
  [[nodiscard]] inline float fillOpacity(const ShellConfig::FrameConfig& cfg, const BarConfig* bar) noexcept {
    if (cfg.matchBar && bar != nullptr) {
      return std::clamp(bar->backgroundOpacity, 0.0f, 1.0f);
    }
    return std::clamp(cfg.opacity, 0.0f, 1.0f);
  }

  // The edge the frame must not paint over, i.e. the one the bar is currently occupying.
  [[nodiscard]] inline Edge cededEdge(const BarConfig* bar, bool barVisible) noexcept {
    return (bar != nullptr && barVisible) ? edgeForPosition(bar->position) : Edge::None;
  }

  // The hole actually painted around: the reserved hole pulled inward by the bleed, so the
  // frame covers the gap the compositor leaves between the reserved area and the window.
  [[nodiscard]] inline Rect paintedHole(float w, float h, const Insets& reserved, int bleed) noexcept {
    const auto b = static_cast<float>(std::max(0, bleed));
    const float x = std::clamp(reserved.left + b, 0.0f, w);
    const float y = std::clamp(reserved.top + b, 0.0f, h);
    return Rect{
        x, y, std::max(0.0f, w - reserved.left - reserved.right - (2.0f * b)),
        std::max(0.0f, h - reserved.top - reserved.bottom - (2.0f * b))
    };
  }

  // A radius past half the hole would make the corner arcs self-intersect.
  [[nodiscard]] inline float clampRadius(int radius, const Rect& hole) noexcept {
    return std::max(0.0f, std::min({static_cast<float>(radius), hole.width * 0.5f, hole.height * 0.5f}));
  }

  // The four frame rails: the output minus the bar's reserved band, minus the painted hole.
  // Disjoint by construction so nothing double-blends at fractional opacity.
  [[nodiscard]] inline std::array<Rect, 4>
  rails(float w, float h, const Insets& reserved, const Rect& hole, Edge barEdge) noexcept {
    float left = 0.0f;
    float top = 0.0f;
    float right = w;
    float bottom = h;
    switch (barEdge) {
    case Edge::Bottom:
      bottom = std::clamp(h - reserved.bottom, 0.0f, h);
      break;
    case Edge::Top:
      top = std::clamp(reserved.top, 0.0f, h);
      break;
    case Edge::Left:
      left = std::clamp(reserved.left, 0.0f, w);
      break;
    case Edge::Right:
      right = std::clamp(w - reserved.right, 0.0f, w);
      break;
    default:
      break;
    }
    const float width = std::max(0.0f, right - left);
    const float holeRight = hole.x + hole.width;
    const float holeBottom = hole.y + hole.height;
    return {
        Rect{left, top, width, hole.y - top},                            // top
        Rect{left, holeBottom, width, bottom - holeBottom},              // bottom
        Rect{left, hole.y, hole.x - left, holeBottom - hole.y},          // left
        Rect{holeRight, hole.y, right - holeRight, holeBottom - hole.y}, // right
    };
  }

  // End caps inside the bar's band. Our reservation strips inset the bar on the two edges
  // perpendicular to it, so it cannot reach the corners; these fill them, in the bar's fill.
  // Sized by the RESERVED inset, never the painted one — bleeding them further would reach
  // past the bar's edge and clip its outermost widget.
  [[nodiscard]] inline std::array<Rect, 2> endCaps(float w, float h, const Insets& reserved, Edge barEdge) noexcept {
    const float bandTop = std::clamp(reserved.top, 0.0f, h);
    const float bandBottom = std::clamp(h - reserved.bottom, 0.0f, h);
    const float bandLeft = std::clamp(reserved.left, 0.0f, w);
    const float bandRight = std::clamp(w - reserved.right, 0.0f, w);
    switch (barEdge) {
    case Edge::Bottom:
      return {
          Rect{0.0f, bandBottom, bandLeft, h - bandBottom},
          Rect{bandRight, bandBottom, w - bandRight, h - bandBottom},
      };
    case Edge::Top:
      return {
          Rect{0.0f, 0.0f, bandLeft, bandTop},
          Rect{bandRight, 0.0f, w - bandRight, bandTop},
      };
    case Edge::Left:
      return {
          Rect{0.0f, 0.0f, bandLeft, bandTop},
          Rect{0.0f, bandBottom, bandLeft, h - bandBottom},
      };
    case Edge::Right:
      return {
          Rect{bandRight, 0.0f, w - bandRight, bandTop},
          Rect{bandRight, bandBottom, w - bandRight, h - bandBottom},
      };
    default:
      return {Rect{}, Rect{}};
    }
  }

  [[nodiscard]] inline bool overlaps(const Rect& a, const Rect& b) noexcept {
    if (a.empty() || b.empty()) {
      return false;
    }
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
  }

} // namespace frame_geometry
