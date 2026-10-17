#include "capture/screenshot_region_gesture.h"

#include "core/input/key_symbols.h"

#include <cmath>

namespace capture {
  namespace {

    constexpr double kHandleMargin = 15.0; // Hitbox size in pixels

    // Right-button resize zones: outer thirds of each axis pick the matching edge or corner.
    [[nodiscard]] DragMode hitTestRegionThirds(double x, double y, double x0, double y0, double x1, double y1) {
      if (x < x0 - kHandleMargin || x > x1 + kHandleMargin || y < y0 - kHandleMargin || y > y1 + kHandleMargin) {
        return DragMode::None;
      }

      const double thirdW = (x1 - x0) / 3.0;
      const double thirdH = (y1 - y0) / 3.0;
      const bool left = x < x0 + thirdW;
      const bool right = x >= x1 - thirdW;
      const bool top = y < y0 + thirdH;
      const bool bottom = y >= y1 - thirdH;

      if (left && top)
        return DragMode::TopLeftCorner;
      if (right && top)
        return DragMode::TopRightCorner;
      if (left && bottom)
        return DragMode::BottomLeftCorner;
      if (right && bottom)
        return DragMode::BottomRightCorner;
      if (top)
        return DragMode::TopEdge;
      if (bottom)
        return DragMode::BottomEdge;
      if (left)
        return DragMode::LeftEdge;
      if (right)
        return DragMode::RightEdge;
      return DragMode::None;
    }

  } // namespace

  DragMode hitTestSelection(double x, double y, double x0, double y0, double x1, double y1) {
    const bool withinX = x >= x0 - kHandleMargin && x <= x1 + kHandleMargin;
    const bool withinY = y >= y0 - kHandleMargin && y <= y1 + kHandleMargin;

    if (!withinX || !withinY)
      return DragMode::None;

    const double distLeft = std::abs(x - x0);
    const double distRight = std::abs(x - x1);
    const double distTop = std::abs(y - y0);
    const double distBottom = std::abs(y - y1);

    bool nearLeft = distLeft <= kHandleMargin;
    bool nearRight = distRight <= kHandleMargin;
    bool nearTop = distTop <= kHandleMargin;
    bool nearBottom = distBottom <= kHandleMargin;

    // Narrow selections can overlap opposing hit zones; bind the pointer to the nearer edge.
    if (nearLeft && nearRight) {
      if (distLeft < distRight)
        nearRight = false;
      else
        nearLeft = false;
    }
    if (nearTop && nearBottom) {
      if (distTop < distBottom)
        nearBottom = false;
      else
        nearTop = false;
    }

    if (nearTop && nearLeft)
      return DragMode::TopLeftCorner;
    if (nearTop && nearRight)
      return DragMode::TopRightCorner;
    if (nearBottom && nearLeft)
      return DragMode::BottomLeftCorner;
    if (nearBottom && nearRight)
      return DragMode::BottomRightCorner;

    if (nearTop && x >= x0 && x <= x1)
      return DragMode::TopEdge;
    if (nearBottom && x >= x0 && x <= x1)
      return DragMode::BottomEdge;
    if (nearLeft && y >= y0 && y <= y1)
      return DragMode::LeftEdge;
    if (nearRight && y >= y0 && y <= y1)
      return DragMode::RightEdge;

    if (x > x0 && x < x1 && y > y0 && y < y1)
      return DragMode::Move;

    return DragMode::None;
  }

  DragMode regionPressGesture(const RegionPress& press) {
    if (press.confirming && (!press.modifierMode || press.modifierHeld)) {
      if (!press.modifierMode) {
        // A plain drag hits edges/corners/interior; outside starts a new selection.
        const DragMode mode = hitTestSelection(press.x, press.y, press.x0, press.y0, press.x1, press.y1);
        return mode == DragMode::None ? DragMode::NewSelection : mode;
      }
      if (!press.rightButton) {
        // Modifier + left drag moves the region from anywhere.
        return DragMode::Move;
      }
      // Modifier + right drag resizes from the outer thirds; the center does nothing.
      return hitTestRegionThirds(press.x, press.y, press.x0, press.y0, press.x1, press.y1);
    }
    return press.rightButton ? DragMode::None : DragMode::NewSelection;
  }

  bool isRegionConfirmKey(std::uint32_t sym, bool spaceModifier) {
    return KeySymbol::isEnter(sym) || (!spaceModifier && KeySymbol::isSpace(sym));
  }

} // namespace capture
