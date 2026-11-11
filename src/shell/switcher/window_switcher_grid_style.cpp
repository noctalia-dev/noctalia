#include "shell/switcher/window_switcher_grid_style.h"

#include "ui/style.h"

#include <algorithm>
#include <cmath>

WindowSwitcherStyleLayout computeWindowSwitcherGridLayout(const WindowSwitcherStyleContext& context) {
  WindowSwitcherStyleLayout layout;
  layout.grid = true;
  layout.gridColumns = std::max<std::size_t>(1, std::min<std::size_t>(5, context.windowCount));
  layout.cards.resize(context.windowCount);

  const float outerMargin = Style::controlHeightSm * context.scale;
  const float maxWidth = std::max(1.0F, context.screenWidth - outerMargin * 2.0F);
  const float maxHeight = std::max(1.0F, context.screenHeight * 0.7F);
  float padding = Style::spaceLg * context.scale;
  float gap = Style::spaceMd * context.scale;
  float cardWidth = Style::windowSwitcherCompactCardWidth * context.scale;
  const float frameInset = Style::spaceXs * context.scale;

  float cardHeight = std::max(0.0F, cardWidth - frameInset * 2.0F) / Style::windowSwitcherPreviewAspect
      + frameInset * 2.0F
      + (context.showCaption ? Style::windowSwitcherCaptionHeight * context.scale : 0.0F);

  layout.countHeight = context.showCount ? Style::controlHeightSm * context.scale : 0.0F;
  float sectionGap = context.showCount ? gap : 0.0F;
  layout.contentScale = std::min({
      1.0F,
      maxWidth
          / (cardWidth * static_cast<float>(layout.gridColumns)
             + gap * static_cast<float>(layout.gridColumns - 1)
             + padding * 2.0F),
      maxHeight / (cardHeight + padding * 2.0F + layout.countHeight + sectionGap),
  });

  padding *= layout.contentScale;
  gap *= layout.contentScale;
  cardWidth *= layout.contentScale;
  cardHeight *= layout.contentScale;
  layout.countHeight *= layout.contentScale;
  sectionGap *= layout.contentScale;
  const float columnStride = cardWidth + gap;
  const float rowStride = cardHeight + gap;
  const float availableHeight = maxHeight - padding * 2.0F - layout.countHeight - sectionGap;
  const std::size_t count = std::max<std::size_t>(1, context.windowCount);
  const std::size_t totalRows = (count + layout.gridColumns - 1) / layout.gridColumns;

  layout.gridRows =
      std::min(totalRows, static_cast<std::size_t>(std::max(1.0F, std::floor((availableHeight + gap) / rowStride))));

  layout.stripWidth = columnStride * static_cast<float>(layout.gridColumns) - gap;
  layout.stripHeight = rowStride * static_cast<float>(layout.gridRows) - gap;
  layout.stripX = padding;
  layout.stripY = padding + layout.countHeight + sectionGap;
  layout.panelWidth = layout.stripWidth + padding * 2.0F;
  layout.panelHeight = layout.stripY + layout.stripHeight + padding;
  const float scale = context.scale * layout.contentScale;
  layout.countWidth = context.showCount ? std::min(layout.stripWidth, Style::controlHeightLg * scale * 2.0F) : 0.0F;
  layout.countX = (layout.panelWidth - layout.countWidth) * 0.5F;
  layout.countY = padding;

  if (context.windowCount == 0) {
    return layout;
  }

  const std::size_t selected = context.selectedIndex % context.windowCount;
  const std::size_t selectedRow = selected / layout.gridColumns;
  layout.gridFirstRow = std::min(context.gridFirstRow, totalRows - layout.gridRows);

  if (selectedRow < layout.gridFirstRow) {
    layout.gridFirstRow = selectedRow;
  } else if (selectedRow >= layout.gridFirstRow + layout.gridRows) {
    layout.gridFirstRow = selectedRow - layout.gridRows + 1;
  }

  layout.gridContentY = -static_cast<float>(layout.gridFirstRow) * rowStride;
  for (std::size_t index = 0; index < context.windowCount; ++index) {
    const std::size_t row = index / layout.gridColumns;
    const std::size_t rowColumns = std::min(layout.gridColumns, context.windowCount - row * layout.gridColumns);
    const float rowOffset = static_cast<float>(layout.gridColumns - rowColumns) * columnStride * 0.5F;
    auto& target = layout.cards[index];

    // The scrolling content node translates these fixed positions into the viewport.
    target.visible = true;
    target.showCaption = context.showCaption;
    target.wideCaption = true;
    target.depth = index == selected ? WindowSwitcherTileDepth::Selected : WindowSwitcherTileDepth::Near;
    target.x = rowOffset + static_cast<float>(index % layout.gridColumns) * columnStride;
    target.y = static_cast<float>(row) * rowStride;
    target.width = cardWidth;
    target.height = cardHeight;
    target.opacity = 1.0F;
    target.zIndex = index == selected ? 2 : 1;
  }

  return layout;
}
