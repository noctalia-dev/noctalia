#include "shell/switcher/window_switcher_carousel_style.h"

#include "ui/style.h"

#include <algorithm>
#include <cmath>

namespace {

  constexpr std::size_t kVisibleCards = 5;
  constexpr float kMinimumLayoutExtent = 1.0F;
  constexpr int kHeightFitPasses = 2;

  [[nodiscard]] float previewFrameHeight(float cardWidth, float scale) noexcept {
    const float frameInset = Style::spaceXs * scale;
    const float previewWidth = std::max(0.0F, cardWidth - frameInset * 2.0F);
    return previewWidth / Style::windowSwitcherPreviewAspect + frameInset * 2.0F;
  }

} // namespace

WindowSwitcherStyleLayout computeWindowSwitcherCarouselLayout(const WindowSwitcherStyleContext& context) {
  WindowSwitcherStyleLayout layout;
  layout.cards.resize(context.windowCount);

  const float screenMargin = Style::controlHeightSm * context.scale;
  const float countHeight = context.showCount ? Style::controlHeightSm * context.scale : 0.0F;
  const float countWidth = context.showCount ? Style::controlHeightLg * context.scale * 2.0F : 0.0F;
  const float sectionGap = context.showCount ? Style::spaceMd * context.scale : 0.0F;
  const float captionHeight = context.showCaption ? Style::windowSwitcherCaptionHeight * context.scale : 0.0F;
  const float captionGap = context.showCaption ? Style::spaceMd * context.scale : 0.0F;
  layout.visibleCards = std::max<std::size_t>(1, std::min(kVisibleCards, context.windowCount));

  const float availableWidth = std::max(kMinimumLayoutExtent, context.screenWidth - screenMargin * 2.0F);
  auto desiredWidth = [&]() {
    const std::size_t sideSlots = layout.visibleCards / 2;
    float width = Style::windowSwitcherSelectedCardWidth;
    if (sideSlots >= 1) {
      width += Style::windowSwitcherNearCardWidth * 2.0F;
    }
    if (sideSlots >= 2) {
      width += Style::windowSwitcherFarCardWidth * 2.0F;
    }
    width -= Style::windowSwitcherCardOverlap * static_cast<float>(sideSlots * 2);
    return width * context.scale;
  };
  while (layout.visibleCards > 3 && availableWidth / desiredWidth() < Style::windowSwitcherNarrowLayoutThreshold) {
    --layout.visibleCards;
  }

  const float widthScale = std::min(1.0F, availableWidth / desiredWidth());
  float selectedWidth = Style::windowSwitcherSelectedCardWidth * context.scale * widthScale;
  float nearWidth = Style::windowSwitcherNearCardWidth * context.scale * widthScale;
  float farWidth = Style::windowSwitcherFarCardWidth * context.scale * widthScale;
  float overlap = Style::windowSwitcherCardOverlap * context.scale * widthScale;

  auto updateExtents = [&]() {
    const std::size_t sideSlots = layout.visibleCards / 2;
    layout.stripWidth = selectedWidth;
    if (sideSlots >= 1) {
      layout.stripWidth += (nearWidth - overlap) * 2.0F;
    }
    if (sideSlots >= 2) {
      layout.stripWidth += (farWidth - overlap) * 2.0F;
    }
    layout.stripHeight = previewFrameHeight(selectedWidth, context.scale) + captionGap + captionHeight;
    layout.panelWidth = layout.stripWidth;
    layout.panelHeight = countHeight + sectionGap + layout.stripHeight;
  };
  updateExtents();

  const float maxPanelHeight = std::max(kMinimumLayoutExtent, context.screenHeight - screenMargin * 2.0F);
  for (int pass = 0; pass < kHeightFitPasses && layout.panelHeight > maxPanelHeight; ++pass) {
    const float availableStripHeight = std::max(kMinimumLayoutExtent, maxPanelHeight - countHeight - sectionGap);
    const float heightScale = std::min(1.0F, availableStripHeight / layout.stripHeight);
    selectedWidth *= heightScale;
    nearWidth *= heightScale;
    farWidth *= heightScale;
    overlap *= heightScale;
    updateExtents();
  }

  layout.countWidth = countWidth;
  layout.countHeight = countHeight;
  layout.countX = (layout.panelWidth - countWidth) * 0.5F;
  layout.countY = 0.0F;
  layout.stripY = countHeight + sectionGap;

  if (context.windowCount == 0) {
    return layout;
  }

  const std::size_t visibleCount = std::min(layout.visibleCards, context.windowCount);
  const std::size_t centerSlot = visibleCount / 2;
  const std::size_t start =
      (context.selectedIndex % context.windowCount + context.windowCount - centerSlot % context.windowCount)
      % context.windowCount;
  const float selectedX = (layout.stripWidth - selectedWidth) * 0.5F;
  for (std::size_t slot = 0; slot < visibleCount; ++slot) {
    const std::size_t windowIndex = (start + slot) % context.windowCount;
    const auto relativeSlot = static_cast<long>(slot) - static_cast<long>(centerSlot);
    const auto distance = static_cast<std::size_t>(std::abs(relativeSlot));
    WindowSwitcherCardTarget& target = layout.cards[windowIndex];
    target.visible = true;
    target.showCaption = context.showCaption && distance == 0;
    target.wideCaption = target.showCaption;
    target.iconPlacement =
        relativeSlot > 0 ? WindowSwitcherHorizontalPlacement::Right : WindowSwitcherHorizontalPlacement::Left;
    target.closePlacement =
        relativeSlot < 0 ? WindowSwitcherHorizontalPlacement::Left : WindowSwitcherHorizontalPlacement::Right;
    target.depth = distance == 0 ? WindowSwitcherTileDepth::Selected
                                 : (distance == 1 ? WindowSwitcherTileDepth::Near : WindowSwitcherTileDepth::Far);
    target.direction = relativeSlot < 0 ? -1.0F : (relativeSlot > 0 ? 1.0F : 0.0F);
    target.zIndex = static_cast<std::int32_t>(3 - std::min<std::size_t>(distance, 3));
    target.opacity = 1.0F;
    if (distance == 0) {
      target.width = selectedWidth;
      target.x = selectedX;
    } else if (distance == 1) {
      target.width = nearWidth;
      target.x = relativeSlot < 0 ? selectedX - nearWidth + overlap : selectedX + selectedWidth - overlap;
    } else {
      target.width = farWidth;
      const float nearLeft = selectedX - nearWidth + overlap;
      const float nearRight = selectedX + selectedWidth - overlap;
      target.x = relativeSlot < 0 ? nearLeft - farWidth + overlap : nearRight + nearWidth - overlap;
    }
    const float previewHeight = previewFrameHeight(target.width, context.scale);
    target.height = previewHeight;
    if (target.showCaption) {
      target.height += captionGap + captionHeight;
    }
    const float selectedPreviewHeight = previewFrameHeight(selectedWidth, context.scale);
    target.y = (selectedPreviewHeight - previewHeight) * 0.5F;
  }
  return layout;
}
