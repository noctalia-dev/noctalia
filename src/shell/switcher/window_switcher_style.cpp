#include "shell/switcher/window_switcher_style.h"

#include <cmath>

namespace {

  constexpr float kComparisonTolerance = 0.5F;

  [[nodiscard]] bool closeEnough(float lhs, float rhs) noexcept { return std::abs(lhs - rhs) < kComparisonTolerance; }

} // namespace

bool WindowSwitcherStyleLayout::sameGeometryAs(const WindowSwitcherStyleLayout& other) const noexcept {
  if (visibleCards != other.visibleCards
      || boxed != other.boxed
      || !closeEnough(panelWidth, other.panelWidth)
      || !closeEnough(panelHeight, other.panelHeight)
      || !closeEnough(stripX, other.stripX)
      || !closeEnough(stripY, other.stripY)
      || !closeEnough(stripWidth, other.stripWidth)
      || !closeEnough(stripHeight, other.stripHeight)
      || !closeEnough(countX, other.countX)
      || !closeEnough(countY, other.countY)
      || !closeEnough(countWidth, other.countWidth)
      || !closeEnough(countHeight, other.countHeight)
      || cards.size() != other.cards.size()) {
    return false;
  }
  for (std::size_t index = 0; index < cards.size(); ++index) {
    const WindowSwitcherCardTarget& card = cards[index];
    const WindowSwitcherCardTarget& otherCard = other.cards[index];
    if (card.visible != otherCard.visible
        || card.showCaption != otherCard.showCaption
        || card.wideCaption != otherCard.wideCaption
        || card.iconPlacement != otherCard.iconPlacement
        || card.closePlacement != otherCard.closePlacement
        || card.depth != otherCard.depth
        || !closeEnough(card.x, otherCard.x)
        || !closeEnough(card.y, otherCard.y)
        || !closeEnough(card.width, otherCard.width)
        || !closeEnough(card.height, otherCard.height)) {
      return false;
    }
  }
  return true;
}
