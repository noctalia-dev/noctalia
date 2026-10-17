#pragma once

#include <cstdint>

namespace capture {

  enum class DragMode {
    None,
    NewSelection,
    TopEdge,
    BottomEdge,
    LeftEdge,
    RightEdge,
    TopLeftCorner,
    TopRightCorner,
    BottomLeftCorner,
    BottomRightCorner,
    Move
  };

  struct RegionPress {
    bool confirming = false;   // a region is awaiting confirmation
    bool modifierMode = false; // region_modifier is not "none"
    bool modifierHeld = false;
    bool rightButton = false;
    double x = 0.0;
    double y = 0.0;
    // Normalized bounds of the region awaiting confirmation.
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
  };

  // Edge, corner, or interior of the region under the pointer; None outside it.
  [[nodiscard]] DragMode hitTestSelection(double x, double y, double x0, double y0, double x1, double y1);

  // Gesture a button press starts; None ignores the press.
  [[nodiscard]] DragMode regionPressGesture(const RegionPress& press);

  // Whether a key press confirms a region with the default outputs. Space stops confirming
  // while it is the move/resize modifier.
  [[nodiscard]] bool isRegionConfirmKey(std::uint32_t sym, bool spaceModifier);

} // namespace capture
