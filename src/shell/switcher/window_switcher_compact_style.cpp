#include "shell/switcher/window_switcher_compact_style.h"

#include "ui/style.h"

#include <algorithm>
#include <cmath>

namespace {

  constexpr std::size_t kVisibleCards = 5;
  constexpr float kMinimumLayoutExtent = 1.0F;

  [[nodiscard]] float previewFrameHeight(float cardWidth, float scale) noexcept {
    const float frameInset = Style::spaceXs * scale;
    const float previewWidth = std::max(0.0F, cardWidth - frameInset * 2.0F);
    return previewWidth / Style::windowSwitcherPreviewAspect + frameInset * 2.0F;
  }

} // namespace

WindowSwitcherStyleLayout computeWindowSwitcherCompactLayout(const WindowSwitcherStyleContext& context) {
  WindowSwitcherStyleLayout layout;
  layout.boxed = true;
  layout.cards.resize(context.windowCount);
  layout.visibleCards = std::max<std::size_t>(1, std::min(kVisibleCards, context.windowCount));

  const float outerMargin = Style::controlHeightSm * context.scale;
  const float panelPadding = Style::spaceLg * context.scale;
  const float cardGap = Style::spaceMd * context.scale;
  const float countHeight = context.showCount ? Style::controlHeightSm * context.scale : 0.0F;
  const float countWidth = context.showCount ? Style::controlHeightLg * context.scale * 2.0F : 0.0F;
  const float sectionGap = context.showCount ? Style::spaceMd * context.scale : 0.0F;
  const float captionHeight = context.showCaption ? Style::windowSwitcherCaptionHeight * context.scale : 0.0F;
  const float captionGap = context.showCaption ? Style::spaceMd * context.scale : 0.0F;
  const float availableWidth = std::max(kMinimumLayoutExtent, context.screenWidth - outerMargin * 2.0F);
  const float gapsWidth = cardGap * static_cast<float>(layout.visibleCards - 1);
  const float desiredCardsWidth =
      Style::windowSwitcherCompactCardWidth * context.scale * static_cast<float>(layout.visibleCards);
  const float widthScale = std::min(
      1.0F, std::max(kMinimumLayoutExtent, availableWidth - panelPadding * 2.0F - gapsWidth) / desiredCardsWidth
  );
  float cardWidth = Style::windowSwitcherCompactCardWidth * context.scale * widthScale;
  float cardHeight = previewFrameHeight(cardWidth, context.scale) + captionGap + captionHeight;
  const float maxPanelHeight = std::max(kMinimumLayoutExtent, context.screenHeight - outerMargin * 2.0F);
  const float fixedHeight = panelPadding * 2.0F + countHeight + sectionGap;
  if (fixedHeight + cardHeight > maxPanelHeight) {
    const float heightScale = std::max(kMinimumLayoutExtent, maxPanelHeight - fixedHeight) / cardHeight;
    cardWidth *= std::min(1.0F, heightScale);
    cardHeight = previewFrameHeight(cardWidth, context.scale) + captionGap + captionHeight;
  }

  layout.stripWidth = cardWidth * static_cast<float>(layout.visibleCards) + gapsWidth;
  layout.stripHeight = cardHeight;
  layout.stripX = panelPadding;
  layout.stripY = panelPadding + countHeight + sectionGap;
  layout.panelWidth = layout.stripWidth + panelPadding * 2.0F;
  layout.panelHeight = layout.stripY + layout.stripHeight + panelPadding;
  layout.countWidth = countWidth;
  layout.countHeight = countHeight;
  layout.countX = (layout.panelWidth - countWidth) * 0.5F;
  layout.countY = panelPadding;

  if (context.windowCount == 0) {
    return layout;
  }

  const std::size_t visibleCount = std::min(layout.visibleCards, context.windowCount);
  const std::size_t centerSlot = visibleCount / 2;
  const std::size_t start =
      (context.selectedIndex % context.windowCount + context.windowCount - centerSlot % context.windowCount)
      % context.windowCount;
  for (std::size_t slot = 0; slot < visibleCount; ++slot) {
    const std::size_t windowIndex = (start + slot) % context.windowCount;
    const auto relativeSlot = static_cast<long>(slot) - static_cast<long>(centerSlot);
    const auto distance = static_cast<std::size_t>(std::abs(relativeSlot));
    WindowSwitcherCardTarget& target = layout.cards[windowIndex];
    target.visible = true;
    target.showCaption = context.showCaption;
    target.wideCaption = true;
    target.depth = distance == 0 ? WindowSwitcherTileDepth::Selected
                                 : (distance == 1 ? WindowSwitcherTileDepth::Near : WindowSwitcherTileDepth::Far);
    target.x = static_cast<float>(slot) * (cardWidth + cardGap);
    target.y = 0.0F;
    target.width = cardWidth;
    target.height = cardHeight;
    target.opacity = 1.0F;
    target.direction = relativeSlot < 0 ? -1.0F : (relativeSlot > 0 ? 1.0F : 0.0F);
    target.zIndex = distance == 0 ? 2 : 1;
  }
  return layout;
}
