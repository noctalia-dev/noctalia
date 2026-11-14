#include "shell/switcher/window_switcher_grid_style.h"

#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {

  constexpr float kMaxPreviewAspect = 2.0F;

} // namespace

WindowSwitcherStyleLayout computeWindowSwitcherGridLayout(const WindowSwitcherStyleContext& context) {
  WindowSwitcherStyleLayout layout;
  layout.grid = true;
  layout.gridColumns = std::max<std::size_t>(1, std::min<std::size_t>(5, context.windowCount));
  layout.cards.resize(context.windowCount);

  const float outerMargin = Style::controlHeightSm * context.scale;
  const float maxWidth = std::max(1.0F, context.screenWidth - outerMargin * 2.0F);
  const float maxHeight = std::max(1.0F, context.screenHeight * 0.75F);
  float padding = Style::spaceLg * context.scale;
  float gap = Style::spaceMd * context.scale;
  const float frameInset = Style::spaceXs * context.scale;
  const float defaultPreviewWidth =
      std::max(0.0F, Style::windowSwitcherCompactCardWidth * context.scale - frameInset * 2.0F);
  const float previewAreaRoot = defaultPreviewWidth / std::sqrt(Style::windowSwitcherPreviewAspect);
  const float captionHeight = context.showCaption ? Style::windowSwitcherCaptionHeight * context.scale : 0.0F;

  // Reserve the largest supported preview.
  // A row should not be affected by previews loading in another row.
  const float previewExtent = previewAreaRoot * std::numbers::sqrt2_v<float>;
  float rowHeight = previewExtent + frameInset * 2.0F + captionHeight;
  const float stripWidth = (previewExtent + frameInset * 2.0F) * static_cast<float>(layout.gridColumns)
      + gap * static_cast<float>(layout.gridColumns - 1);
  const std::size_t count = std::max<std::size_t>(1, context.windowCount);
  const std::size_t totalRows = (count + layout.gridColumns - 1) / layout.gridColumns;

  layout.countHeight = context.showCount ? Style::controlHeightSm * context.scale : 0.0F;
  float sectionGap = context.showCount ? gap : 0.0F;
  layout.contentScale = std::min({
      1.0F,
      maxWidth / (stripWidth + padding * 2.0F),
      maxHeight / (rowHeight + padding * 2.0F + layout.countHeight + sectionGap),
  });

  padding *= layout.contentScale;
  gap *= layout.contentScale;
  rowHeight *= layout.contentScale;
  layout.countHeight *= layout.contentScale;
  sectionGap *= layout.contentScale;
  const float availableHeight = maxHeight - padding * 2.0F - layout.countHeight - sectionGap;
  const float rowStride = rowHeight + gap;
  layout.gridRows =
      std::min(totalRows, static_cast<std::size_t>(std::max(1.0F, std::floor((availableHeight + gap) / rowStride))));

  layout.stripWidth = stripWidth * layout.contentScale;
  // Only show complete reserved row bands, including the gaps between them.
  layout.stripHeight = static_cast<float>(layout.gridRows) * rowStride - gap;
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

  const float scaledPreviewAreaRoot = previewAreaRoot * layout.contentScale;
  const float framePadding = frameInset * 2.0F * layout.contentScale;
  const float scaledCaptionHeight = captionHeight * layout.contentScale;
  for (std::size_t row = 0; row < totalRows; ++row) {
    const std::size_t first = row * layout.gridColumns;
    const std::size_t end = std::min(first + layout.gridColumns, context.windowCount);
    float rowWidth = gap * static_cast<float>(end - first - 1);
    for (std::size_t index = first; index < end; ++index) {
      float aspect = index < context.windowAspectRatios.size() ? context.windowAspectRatios[index] : 1.0F;
      if (!std::isfinite(aspect) || aspect <= 0.0F) {
        aspect = 1.0F;
      }

      // Very narrow/wide windows are contained inside the reserved bounds.
      const float aspectRoot = std::sqrt(std::clamp(aspect, 1.0F / kMaxPreviewAspect, kMaxPreviewAspect));
      auto& target = layout.cards[index];
      target.width = scaledPreviewAreaRoot * aspectRoot + framePadding;
      target.height = scaledPreviewAreaRoot / aspectRoot + framePadding + scaledCaptionHeight;
      rowWidth += target.width;
    }

    float cardX = (layout.stripWidth - rowWidth) * 0.5F;
    const float rowY = static_cast<float>(row) * rowStride;
    for (std::size_t index = first; index < end; ++index) {
      auto& target = layout.cards[index];
      // The scrolling content node translates these fixed positions into the viewport.
      target.visible = true;
      target.showCaption = context.showCaption;
      target.wideCaption = true;
      target.depth = index == selected ? WindowSwitcherTileDepth::Selected : WindowSwitcherTileDepth::Near;
      target.x = cardX;
      target.y = rowY + (rowHeight - target.height) * 0.5F;
      target.opacity = 1.0F;
      target.zIndex = index == selected ? 2 : 1;
      cardX += target.width + gap;
    }
  }

  return layout;
}
