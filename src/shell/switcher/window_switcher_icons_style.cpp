#include "shell/switcher/window_switcher_icons_style.h"

#include "ui/style.h"

#include <algorithm>
#include <cmath>

WindowSwitcherStyleLayout computeWindowSwitcherIconsLayout(const WindowSwitcherStyleContext& context) {
  WindowSwitcherStyleLayout layout;
  layout.boxed = true;
  layout.cards.resize(context.windowCount);
  layout.visibleCards = context.windowCount;

  const float outerMargin = Style::controlHeightSm * context.scale;
  const float padding = Style::spaceMd * context.scale;
  const float gap = Style::spaceSm * context.scale;
  const float cardSize = Style::windowSwitcherIconCardSize * context.scale;
  const float countHeight = context.showCount ? Style::controlHeightSm * context.scale : 0.0F;
  const float captionHeight = context.showCaption ? Style::controlHeightSm * context.scale : 0.0F;
  const float availableWidth = std::max(1.0F, context.screenWidth - outerMargin * 2.0F);
  const float availableHeight = std::max(1.0F, context.screenHeight - outerMargin * 2.0F);
  const std::size_t count = std::max<std::size_t>(1, context.windowCount);
  const auto fittingColumns =
      static_cast<std::size_t>(std::max(1.0F, std::floor((availableWidth - padding * 2.0F + gap) / (cardSize + gap))));
  const std::size_t columns = std::min(count, fittingColumns);
  const std::size_t rows = count / columns + (count % columns != 0 ? 1 : 0);
  const float desiredWidth = context.windowCount == 0
      ? Style::controlHeightLg * context.scale * 4.0F
      : static_cast<float>(columns) * cardSize + static_cast<float>(columns - 1) * gap;
  const float desiredHeight = static_cast<float>(rows) * cardSize + static_cast<float>(rows - 1) * gap;
  const float fixedHeight = padding * 2.0F
      + countHeight
      + captionHeight
      + (context.showCount ? gap : 0.0F)
      + (context.showCaption ? gap : 0.0F);
  const float fit = std::min(
      {1.0F, std::max(1.0F, availableWidth - padding * 2.0F) / desiredWidth,
       std::max(1.0F, availableHeight - fixedHeight) / desiredHeight}
  );
  const float size = cardSize * fit;
  const float spacing = gap * fit;

  layout.stripWidth = desiredWidth * fit;
  layout.stripHeight = desiredHeight * fit;
  layout.stripX = padding;
  layout.stripY = padding + countHeight + (context.showCount ? gap : 0.0F);
  layout.panelWidth = layout.stripWidth + padding * 2.0F;
  layout.panelHeight =
      layout.stripY + layout.stripHeight + padding + captionHeight + (context.showCaption ? gap : 0.0F);
  layout.countWidth =
      context.showCount ? std::min(layout.stripWidth, Style::controlHeightLg * context.scale * 2.0F) : 0.0F;
  layout.countHeight = countHeight;
  layout.countX = (layout.panelWidth - layout.countWidth) * 0.5F;
  layout.countY = padding;

  for (std::size_t index = 0; index < context.windowCount; ++index) {
    WindowSwitcherCardTarget& target = layout.cards[index];
    target.visible = true;
    target.depth = index == context.selectedIndex ? WindowSwitcherTileDepth::Selected : WindowSwitcherTileDepth::Near;
    target.x = static_cast<float>(index % columns) * (size + spacing);
    target.y = static_cast<float>(index / columns) * (size + spacing);
    target.width = size;
    target.height = size;
    target.opacity = 1.0F;
    target.zIndex = index == context.selectedIndex ? 2 : 1;
  }
  return layout;
}
