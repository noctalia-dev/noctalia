#include "ui/controls/virtual_grid_view.h"

#include "render/animation/animation.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_area.h"
#include "ui/controls/scroll_view.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <memory>
#include <utility>

// Internal canvas that reports a virtual size set externally and never moves
// its children during its own layout pass — VirtualGridView positions pool
// tiles and the overlay InputArea explicitly.
class VirtualGridView::Canvas : public Node {
public:
  Canvas() = default;

  void setVirtualSize(float width, float height) {
    m_virtualWidth = std::max(0.0F, width);
    m_virtualHeight = std::max(0.0F, height);
    setSize(m_virtualWidth, m_virtualHeight);
  }

protected:
  LayoutSize doMeasure(Renderer& /*renderer*/, const LayoutConstraints& constraints) override {
    return constraints.constrain(LayoutSize{.width = m_virtualWidth, .height = m_virtualHeight});
  }

  void doLayout(Renderer& /*renderer*/) override {
    // Tiles and the input overlay are positioned by VirtualGridView::doLayout.
  }

  void doArrange(Renderer& /*renderer*/, const LayoutRect& rect) override {
    setPosition(rect.x, rect.y);
    setSize(m_virtualWidth, m_virtualHeight);
  }

private:
  float m_virtualWidth = 0.0F;
  float m_virtualHeight = 0.0F;
};

VirtualGridView::VirtualGridView() {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(0.0F);
  setPadding(0.0F);
  setFillWidth(true);
  setFillHeight(true);

  auto scroll = std::make_unique<ScrollView>();
  scroll->setFlexGrow(1.0F);
  scroll->setViewportPaddingH(0.0F);
  scroll->setViewportPaddingV(0.0F);
  scroll->setOnScrollChanged([this](float offset) { onScrollChanged(offset); });
  m_scroll = static_cast<ScrollView*>(addChild(std::move(scroll)));

  auto canvas = std::make_unique<Canvas>();
  m_canvas = static_cast<Canvas*>(m_scroll->content()->addChild(std::move(canvas)));

  auto rubberBand = std::make_unique<Flex>();
  rubberBand->setParticipatesInLayout(false);
  rubberBand->setHitTestVisible(false);
  rubberBand->setZIndex(40);
  constexpr float selectionFillOpacity = 0.16F;
  constexpr float selectionBorderOpacity = 0.85F;
  rubberBand->setFill(colorSpecFromRole(ColorRole::Primary, selectionFillOpacity));
  rubberBand->setBorder(colorSpecFromRole(ColorRole::Primary, selectionBorderOpacity), 1.0F);
  rubberBand->setVisible(false);
  m_rubberBand = static_cast<Flex*>(m_canvas->addChild(std::move(rubberBand)));

  auto inputArea = std::make_unique<InputArea>();
  inputArea->setZIndex(50);
  inputArea->setFocusable(true);
  inputArea->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
  inputArea->setOnEnter([this](const InputArea::PointerData& data) { onPointerEnter(data.localX, data.localY); });
  inputArea->setOnMotion([this](const InputArea::PointerData& data) { onPointerMotion(data.localX, data.localY); });
  inputArea->setOnLeave([this] { onPointerLeave(); });
  inputArea->setOnPress([this](const InputArea::PointerData& data) {
    if (data.button == BTN_LEFT) {
      if (data.pressed) {
        onPointerPress(data.localX, data.localY);
      } else {
        onPointerRelease(data.localX, data.localY);
      }
    } else if (data.pressed && data.button == BTN_RIGHT) {
      onSecondaryPointerPress(data.localX, data.localY);
    }
  });
  inputArea->setOnCancel([this] {
    if (m_adapterPointerCapture && m_adapter != nullptr) {
      m_adapter->onPointerCancel();
    }
    m_adapterPointerCapture = false;
  });
  m_inputArea = static_cast<InputArea*>(m_canvas->addChild(std::move(inputArea)));
}

void VirtualGridView::bindScrollState(ScrollViewState* state) { m_scroll->bindState(state); }

void VirtualGridView::setAdapter(VirtualGridAdapter* adapter) {
  if (m_adapter == adapter) {
    return;
  }
  if (m_adapterPointerCapture && m_adapter != nullptr) {
    m_adapter->onPointerCancel();
  }
  endRubberBand(true);
  m_adapter = adapter;
  if (animationManager() != nullptr && m_selectionAnimationId != 0) {
    animationManager()->cancel(m_selectionAnimationId);
    m_selectionAnimationId = 0;
  }
  m_adapterPointerCapture = false;
  // Drop the existing pool — tiles were built by the previous adapter's createTile().
  for (Node* tile : m_pool) {
    if (tile != nullptr) {
      m_canvas->removeChild(tile);
    }
  }
  for (InputArea* area : m_poolTooltipAreas) {
    if (area != nullptr) {
      m_inputArea->removeChild(area);
    }
  }
  m_poolTooltipAreas.clear();
  m_pool.clear();
  m_slotBoundIndex.clear();
  m_selectedIndex.reset();
  m_selectionIndicatorSnapPending = true;
  if (m_selectionIndicator != nullptr) {
    m_selectionIndicator->setVisible(false);
  }
  m_hoveredIndex.reset();
  m_hoveredOverlayIndex.reset();
  markLayoutDirty();
}

void VirtualGridView::notifyDataChanged() {
  if (animationManager() != nullptr && m_selectionAnimationId != 0) {
    animationManager()->cancel(m_selectionAnimationId);
    m_selectionAnimationId = 0;
  }
  m_selectionIndicatorSnapPending = true;
  if (m_selectionIndicator != nullptr) {
    m_selectionIndicator->setVisible(false);
  }
  // Force every visible slot to rebind on the next layout.
  std::ranges::fill(m_slotBoundIndex, std::nullopt);
  markLayoutDirty();
}

void VirtualGridView::setSelectionIndicator(std::unique_ptr<Node> indicator) {
  if (animationManager() != nullptr && m_selectionAnimationId != 0) {
    animationManager()->cancel(m_selectionAnimationId);
    m_selectionAnimationId = 0;
  }
  if (m_selectionIndicator != nullptr) {
    (void)m_canvas->removeChild(m_selectionIndicator);
    m_selectionIndicator = nullptr;
  }
  if (indicator == nullptr || m_canvas == nullptr) {
    return;
  }
  indicator->setHitTestVisible(false);
  indicator->setZIndex(-1);
  indicator->setVisible(false);
  m_selectionIndicator = m_canvas->addChild(std::move(indicator));
  m_selectionIndicatorSnapPending = true;
  markLayoutDirty();
}

void VirtualGridView::setSelectionIndicatorEnabled(bool enabled) {
  if (m_selectionIndicatorEnabled == enabled) {
    return;
  }
  m_selectionIndicatorEnabled = enabled;
  if (!enabled && animationManager() != nullptr && m_selectionAnimationId != 0) {
    animationManager()->cancel(m_selectionAnimationId);
    m_selectionAnimationId = 0;
  }
  if (m_selectionIndicator != nullptr) {
    m_selectionIndicator->setVisible(false);
  }
  m_selectionIndicatorSnapPending = true;
  markLayoutDirty();
}

void VirtualGridView::notifyItemChanged(std::size_t index) {
  for (auto& boundIndex : m_slotBoundIndex) {
    if (boundIndex.has_value() && *boundIndex == index) {
      boundIndex.reset();
      markLayoutDirty();
      return;
    }
  }
}

void VirtualGridView::setColumns(std::size_t columns) {
  if (m_columns == columns) {
    return;
  }
  m_columns = columns;
  notifyDataChanged();
}

void VirtualGridView::setMinCellWidth(float width) {
  m_minCellWidth = std::max(1.0F, width);
  notifyDataChanged();
}

void VirtualGridView::setCellHeight(float height) {
  const float next = std::max(1.0F, height);
  if (std::abs(m_cellHeight - next) < 0.001F) {
    return;
  }
  m_cellHeight = next;
  notifyDataChanged();
}

void VirtualGridView::setSquareCells(bool square) {
  if (m_squareCells == square) {
    return;
  }
  m_squareCells = square;
  notifyDataChanged();
}

void VirtualGridView::setColumnGap(float gap) {
  m_columnGap = std::max(0.0F, gap);
  notifyDataChanged();
}

void VirtualGridView::setRowGap(float gap) {
  m_rowGap = std::max(0.0F, gap);
  notifyDataChanged();
}

void VirtualGridView::setOverscanRows(std::size_t rows) {
  m_overscanRows = rows;
  markLayoutDirty();
}

void VirtualGridView::setScale(float scale) {
  m_scale = std::max(0.1F, scale);
  if (m_scroll != nullptr) {
    m_scroll->setContentScale(m_scale);
  }
}

void VirtualGridView::setItemCursorShape(std::uint32_t shape) {
  if (m_itemCursorShape == shape) {
    return;
  }
  m_itemCursorShape = shape;
  for (InputArea* area : m_poolTooltipAreas) {
    if (area != nullptr) {
      area->setCursorShape(shape);
    }
  }
}

void VirtualGridView::scrollToIndex(std::size_t index) {
  m_pendingScrollToIndex = true;
  m_pendingScrollIndex = index;
  markLayoutDirty();
}

InputArea* VirtualGridView::beginInlineEdit(
    std::size_t index, const std::string& path, const std::string& name, std::function<void(const std::string&)> submit,
    std::function<void()> cancel
) {
  if (m_adapter == nullptr)
    return nullptr;
  for (std::size_t slot = 0; slot < m_pool.size(); ++slot) {
    if (slot < m_slotBoundIndex.size() && m_slotBoundIndex[slot] == index && m_pool[slot] != nullptr)
      return m_adapter->beginInlineEdit(*m_pool[slot], path, name, std::move(submit), std::move(cancel));
  }
  return nullptr;
}

void VirtualGridView::endInlineEdit(const std::string& path) {
  if (m_adapter == nullptr)
    return;
  for (Node* tile : m_pool)
    if (tile != nullptr)
      m_adapter->endInlineEdit(*tile, path);
}

void VirtualGridView::setSelectedIndex(std::optional<std::size_t> index, bool scrollToSelection) {
  if (!scrollToSelection) {
    m_pendingScrollToIndex = false;
  }
  if (m_selectedIndex == index) {
    if (scrollToSelection && m_selectedIndex.has_value()) {
      m_pendingScrollToIndex = true;
      m_pendingScrollIndex = *m_selectedIndex;
      markLayoutDirty();
    }
    return;
  }
  const std::optional<std::size_t> previousIndex = m_selectedIndex;
  bool animateIndicator = false;
  if (m_selectionIndicator != nullptr
      && m_selectionIndicatorEnabled
      && !m_selectionIndicatorSnapPending
      && previousIndex.has_value()
      && index.has_value()
      && m_layoutColumns == 1
      && *previousIndex < m_itemCount
      && *index < m_itemCount
      && (*previousIndex + 1 == *index || *index + 1 == *previousIndex)) {
    const float stride = m_cellHeightResolved + m_rowGap;
    const float visibleTop = m_scroll != nullptr ? m_scroll->scrollOffset() : 0.0F;
    const float visibleBottom = visibleTop + (m_scroll != nullptr ? m_scroll->contentViewportHeight() : 0.0F);
    const float previousTop = static_cast<float>(*previousIndex) * stride;
    const float nextTop = static_cast<float>(*index) * stride;
    animateIndicator = previousTop >= visibleTop
        && previousTop + m_cellHeightResolved <= visibleBottom
        && nextTop >= visibleTop
        && nextTop + m_cellHeightResolved <= visibleBottom;
  }
  m_selectedIndex = index;
  if (scrollToSelection && m_selectedIndex.has_value()) {
    m_pendingScrollToIndex = true;
    m_pendingScrollIndex = *m_selectedIndex;
  } else {
    m_pendingScrollToIndex = false;
    m_pendingScrollIndex = 0;
  }
  if (m_selectionIndicator != nullptr) {
    if (!m_selectionIndicatorEnabled || !index.has_value()) {
      if (animationManager() != nullptr && m_selectionAnimationId != 0) {
        animationManager()->cancel(m_selectionAnimationId);
        m_selectionAnimationId = 0;
      }
      m_selectionIndicator->setVisible(false);
      m_selectionIndicatorSnapPending = false;
    } else if (animateIndicator && animationManager() != nullptr) {
      const float targetY = static_cast<float>(*index) * (m_cellHeightResolved + m_rowGap);
      const float fromY = m_selectionIndicator->visible()
          ? m_selectionIndicator->y()
          : static_cast<float>(*previousIndex) * (m_cellHeightResolved + m_rowGap);
      if (m_selectionAnimationId != 0) {
        animationManager()->cancel(m_selectionAnimationId);
      }
      m_selectionIndicator->setPosition(0.0F, fromY);
      m_selectionIndicator->setSize(m_cellWidth, m_cellHeightResolved);
      m_selectionIndicator->setVisible(true);
      m_selectionIndicatorSnapPending = false;
      m_selectionAnimationId = animationManager()->animate(
          fromY, targetY, 140.0F, Easing::EaseOutCubic,
          [this](float y) {
            if (m_selectionIndicator != nullptr) {
              m_selectionIndicator->setPosition(0.0F, y);
            }
          },
          [this] { m_selectionAnimationId = 0; }, m_selectionIndicator
      );
    } else {
      if (animationManager() != nullptr && m_selectionAnimationId != 0) {
        animationManager()->cancel(m_selectionAnimationId);
        m_selectionAnimationId = 0;
      }
      m_selectionIndicatorSnapPending = true;
    }
  }
  markLayoutDirty();
  if (m_onSelectionChanged) {
    m_onSelectionChanged(m_selectedIndex);
  }
}

void VirtualGridView::setOnSelectionChanged(std::function<void(std::optional<std::size_t>)> callback) {
  m_onSelectionChanged = std::move(callback);
}

std::size_t VirtualGridView::pageItemStride() const noexcept {
  if (m_scroll == nullptr || m_layoutColumns == 0 || m_cellHeightResolved <= 0.0F) {
    return 1;
  }
  const float viewportH = m_scroll->contentViewportHeight();
  const float rowStride = m_cellHeightResolved + m_rowGap;
  if (rowStride <= 0.0F) {
    return m_layoutColumns;
  }
  const auto visibleRows = std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(viewportH / rowStride)));
  return std::max<std::size_t>(1, visibleRows * m_layoutColumns);
}

void VirtualGridView::doLayout(Renderer& renderer) {
  if (m_adapter == nullptr || m_scroll == nullptr || m_canvas == nullptr || m_inputArea == nullptr) {
    Flex::doLayout(renderer);
    return;
  }
  if (m_selectionIndicator != nullptr
      && animationManager() != nullptr
      && m_selectionAnimationId != 0
      && m_selectionIndicatorSnapPending) {
    animationManager()->cancel(m_selectionAnimationId);
    m_selectionAnimationId = 0;
  }

  // Step 1: estimate the canvas's virtual size so ScrollView can measure
  // content height correctly. Match ScrollView's configured gutter behavior.
  const float ourW = std::max(0.0F, width());
  const float ourH = std::max(0.0F, height());
  const float padH = m_scroll->viewportPaddingH();
  const float padV = m_scroll->viewportPaddingV();
  const float innerW = std::max(0.0F, ourW - 2.0F * padH);
  const float viewportH = std::max(0.0F, ourH - 2.0F * padV);
  const float scrollbarGutter = m_scroll->scrollbarGutter();

  if (m_rubberBandPending && m_itemCount != m_adapter->itemCount())
    endRubberBand(true);
  m_itemCount = m_adapter->itemCount();

  struct GridMetrics {
    std::size_t columns = 1;
    float cellW = 0.0F;
    float cellH = 0.0F;
    std::size_t rowCount = 0;
    float virtualHeight = 0.0F;
  };

  auto resolveMetrics = [this](float availableW) {
    GridMetrics metrics;
    metrics.columns = m_columns > 0
        ? std::max<std::size_t>(1, m_columns)
        : std::max<std::size_t>(
              1,
              static_cast<std::size_t>(
                  std::floor((availableW + m_columnGap) / std::max(1.0F, m_minCellWidth + m_columnGap))
              )
          );
    const auto columnsF = static_cast<float>(metrics.columns);
    metrics.cellW = std::max(0.0F, (availableW - (columnsF - 1.0F) * m_columnGap) / std::max(1.0F, columnsF));
    metrics.cellH = m_squareCells ? metrics.cellW : m_cellHeight;
    metrics.rowCount = (m_itemCount + metrics.columns - 1) / metrics.columns;
    metrics.virtualHeight = metrics.rowCount == 0
        ? 0.0F
        : (static_cast<float>(metrics.rowCount) * metrics.cellH + static_cast<float>(metrics.rowCount - 1) * m_rowGap);
    return metrics;
  };

  float viewportW = innerW;
  GridMetrics metrics = resolveMetrics(viewportW);
  if (m_scroll->reservesScrollbarGutter() || metrics.virtualHeight > viewportH + 0.5F) {
    viewportW = std::max(0.0F, innerW - scrollbarGutter);
    metrics = resolveMetrics(viewportW);
  }

  const std::size_t previousLayoutColumns = m_layoutColumns;
  const std::size_t columns = metrics.columns;
  const float cellW = metrics.cellW;
  const float cellH = metrics.cellH;
  const std::size_t rowCount = metrics.rowCount;
  const float virtualHeight = metrics.virtualHeight;

  if (m_selectionIndicator != nullptr
      && (std::abs(cellW - m_indicatorCellWidth) >= 0.5F
          || std::abs(cellH - m_indicatorCellHeight) >= 0.5F
          || columns != m_indicatorColumns)) {
    if (animationManager() != nullptr && m_selectionAnimationId != 0) {
      animationManager()->cancel(m_selectionAnimationId);
      m_selectionAnimationId = 0;
    }
    m_selectionIndicatorSnapPending = true;
  }
  m_indicatorCellWidth = cellW;
  m_indicatorCellHeight = cellH;
  m_indicatorColumns = columns;

  if (m_rubberBandPending && (columns != m_layoutColumns || cellW != m_cellWidth || cellH != m_cellHeightResolved)) {
    endRubberBand(true);
  }
  m_layoutColumns = columns;
  m_cellWidth = cellW;
  m_cellHeightResolved = cellH;
  m_virtualWidth = viewportW;
  m_virtualHeight = virtualHeight;

  m_canvas->setVirtualSize(viewportW, std::max(viewportH, virtualHeight));

  // Apply any pending scrollToIndex now that we know cell geometry. Keep the
  // current viewport stable when the target row is already fully visible; this
  // makes keyboard navigation scroll only at the viewport edges instead of
  // pinning each selected row to the top.
  if (m_pendingScrollToIndex) {
    m_pendingScrollToIndex = false;
    if (m_pendingScrollIndex < m_itemCount && columns > 0) {
      const std::size_t row = m_pendingScrollIndex / columns;
      const float rowTop = static_cast<float>(row) * (cellH + m_rowGap);
      const float rowBottom = rowTop + cellH;
      const float visibleTop = m_scroll->scrollOffset();
      const float visibleBottom = visibleTop + viewportH;
      if (rowTop < visibleTop) {
        m_scroll->requestScrollToOffset(rowTop);
      } else if (rowBottom > visibleBottom) {
        m_scroll->requestScrollToOffset(rowBottom - viewportH);
      }
    }
  }

  // Resolve pending jumps and clamp the offset against the new canvas before binding tiles.
  Flex::doLayout(renderer);

  if (m_selectionIndicator != nullptr && m_selectionIndicatorSnapPending) {
    if (m_selectionIndicatorEnabled && m_selectedIndex.has_value() && *m_selectedIndex < m_itemCount) {
      const auto row = *m_selectedIndex / std::max<std::size_t>(1, columns);
      m_selectionIndicator->setPosition(0.0F, static_cast<float>(row) * (cellH + m_rowGap));
      m_selectionIndicator->setSize(cellW, cellH);
      m_selectionIndicator->setVisible(true);
    } else {
      m_selectionIndicator->setVisible(false);
    }
    m_selectionIndicatorSnapPending = false;
  }

  // Step 2: determine the visible row range from the resolved scroll offset, with overscan.
  const float rowStride = cellH + m_rowGap;
  const float scrollY = m_scroll->scrollOffset();
  std::size_t firstRow = 0;
  std::size_t lastRow = 0;
  if (rowStride > 0.0F && rowCount > 0) {
    const long firstRaw = static_cast<long>(std::floor(scrollY / rowStride)) - static_cast<long>(m_overscanRows);
    const long lastRaw =
        static_cast<long>(std::ceil((scrollY + viewportH) / rowStride)) + static_cast<long>(m_overscanRows);
    firstRow = static_cast<std::size_t>(std::max<long>(0, firstRaw));
    lastRow = static_cast<std::size_t>(std::max<long>(0, std::min<long>(lastRaw, static_cast<long>(rowCount) - 1)));
  }
  const std::size_t visibleRowCount = (rowCount == 0 || lastRow < firstRow) ? 0 : (lastRow - firstRow + 1);
  const std::size_t desiredPoolSize = visibleRowCount * columns;

  // Step 3: grow the pool if needed. Pool is row-major (poolRows * columns)
  // so we can address slots by `row % poolRows` and keep tiles bound to the
  // same logical index while they remain in the visible window — avoids the
  // "every scroll rebinds every slot" thrash that would otherwise release
  // expensive per-tile state (e.g. wallpaper thumbnails).
  if (previousLayoutColumns != columns) {
    // Column count changed (resize): existing slot→logicalIndex mapping is stale.
    std::ranges::fill(m_slotBoundIndex, std::nullopt);
  }
  while (m_pool.size() < desiredPoolSize) {
    const std::size_t slot = m_pool.size();
    auto tile = m_adapter->createTile();
    if (tile == nullptr) {
      break;
    }
    tile->setVisible(false);
    tile->setParticipatesInLayout(false);
    Node* raw = m_canvas->addChild(std::move(tile));
    m_pool.push_back(raw);
    m_slotBoundIndex.emplace_back();
    m_slotBoundSelected.push_back(false);
    m_slotBoundHovered.push_back(false);
    m_slotBoundOverlayHovered.push_back(false);

    // Per-cell overlay: carries the item tooltip and the item cursor shape.
    auto tooltipArea = std::make_unique<InputArea>();
    tooltipArea->setVisible(false);
    tooltipArea->setAcceptedButtons(0);
    tooltipArea->setCursorShape(m_itemCursorShape);
    tooltipArea->setOnEnter([this, slot](const InputArea::PointerData& data) {
      onPoolTooltipMotion(slot, data.localX, data.localY);
    });
    tooltipArea->setOnMotion([this, slot](const InputArea::PointerData& data) {
      onPoolTooltipMotion(slot, data.localX, data.localY);
    });
    tooltipArea->setOnLeave([this, slot] { onPoolTooltipLeave(slot); });
    tooltipArea->setTooltipProvider([this, slot] -> TooltipContent {
      if (m_tooltipSuppressed
          || m_adapter == nullptr
          || slot >= m_slotBoundIndex.size()
          || !m_slotBoundIndex[slot].has_value()) {
        return {};
      }
      const std::size_t index = *m_slotBoundIndex[slot];
      std::string tooltip = m_slotBoundOverlayHovered[slot] ? m_adapter->overlayTooltip(index) : std::string{};
      if (tooltip.empty()) {
        tooltip = m_adapter->itemTooltip(index);
      }
      return tooltip.empty() ? TooltipContent{} : TooltipContent{std::move(tooltip)};
    });
    m_poolTooltipAreas.push_back(static_cast<InputArea*>(m_inputArea->addChild(std::move(tooltipArea))));
  }

  // Step 4: bind / position pool slots, addressing by `row % poolRows`.
  const std::size_t poolRows = columns == 0 ? 0 : m_pool.size() / columns;
  m_visibleStartIndex = firstRow * columns;
  std::vector<bool> slotActive(m_pool.size(), false);

  if (poolRows > 0 && visibleRowCount > 0) {
    for (std::size_t row = firstRow; row <= lastRow; ++row) {
      const std::size_t poolRow = row % poolRows;
      for (std::size_t col = 0; col < columns; ++col) {
        const std::size_t slot = poolRow * columns + col;
        if (slot >= m_pool.size()) {
          continue;
        }
        const std::size_t logicalIndex = row * columns + col;
        if (logicalIndex >= m_itemCount) {
          continue;
        }
        Node* tile = m_pool[slot];
        if (tile == nullptr) {
          continue;
        }

        slotActive[slot] = true;

        const float x = static_cast<float>(visualCol(col)) * (cellW + m_columnGap);
        const float y = static_cast<float>(row) * (cellH + m_rowGap);
        const bool geometryChanged = tile->width() != cellW || tile->height() != cellH;
        tile->setPosition(x, y);
        tile->setSize(cellW, cellH);

        InputArea* tooltipArea = m_poolTooltipAreas[slot];
        tooltipArea->setPosition(x, y);
        tooltipArea->setFrameSize(cellW, cellH);
        tooltipArea->setVisible(true);
        const bool selected = m_adapter->selectedState(logicalIndex)
                                  .value_or(m_selectedIndex.has_value() && *m_selectedIndex == logicalIndex);
        const bool hovered = m_hoveredIndex.has_value() && *m_hoveredIndex == logicalIndex;
        const bool overlayHovered = m_hoveredOverlayIndex.has_value() && *m_hoveredOverlayIndex == logicalIndex;
        updateTooltipAnchor(slot, logicalIndex, overlayHovered);
        const bool rebound = m_slotBoundIndex[slot] != logicalIndex;
        const bool dirty = !m_slotBoundIndex[slot].has_value()
            || geometryChanged
            || *m_slotBoundIndex[slot] != logicalIndex
            || m_slotBoundSelected[slot] != selected
            || m_slotBoundHovered[slot] != hovered;
        if (dirty) {
          m_adapter->bindTile(*tile, logicalIndex, selected, hovered);
          m_slotBoundIndex[slot] = logicalIndex;
          m_slotBoundSelected[slot] = selected;
          m_slotBoundHovered[slot] = hovered;
          if (rebound)
            tooltipArea->requestTooltipRefresh();
        }
        if (m_slotBoundOverlayHovered[slot] != overlayHovered) {
          m_adapter->applyOverlayHover(*tile, overlayHovered);
          m_slotBoundOverlayHovered[slot] = overlayHovered;
        }
        tile->setVisible(true);
        // Canvas's doLayout is a no-op, so we lay out each pool tile explicitly
        // here — otherwise the inner glyph/label children never get measured.
        tile->layout(renderer);
      }
    }
  }
  if (m_selectionIndicator != nullptr && m_selectionIndicator->visible()) {
    m_selectionIndicator->layout(renderer);
  }

  for (std::size_t slot = 0; slot < m_pool.size(); ++slot) {
    if (!slotActive[slot] && m_pool[slot] != nullptr) {
      m_pool[slot]->setVisible(false);
      m_slotBoundIndex[slot].reset();
      if (slot < m_poolTooltipAreas.size() && m_poolTooltipAreas[slot] != nullptr) {
        m_poolTooltipAreas[slot]->setVisible(false);
        m_poolTooltipAreas[slot]->requestTooltipRefresh();
      }
      m_slotBoundOverlayHovered[slot] = false;
    }
  }

  // Step 5: position the input overlay across the entire virtual canvas.
  m_inputArea->setPosition(0.0F, 0.0F);
  m_inputArea->setFrameSize(viewportW, std::max(viewportH, virtualHeight));
  if (m_rubberBand->visible())
    m_rubberBand->layout(renderer);
}

LayoutSize VirtualGridView::doMeasure(Renderer& /*renderer*/, const LayoutConstraints& constraints) {
  // Pure size report: never run doLayout from measure. The parent's measure pass
  // happens with intermediate constraints (often width=0 for a fillWidth+flexGrow
  // child); binding tiles at those phantom widths would then mark layout dirty and
  // schedule another full root re-layout, which manifests as ~6px wobble on
  // sibling labels (header titles, columns) every time the cursor moves anywhere
  // in the dialog. Tile binding belongs in the arrange pass where the final rect
  // is known.
  const float w = constraints.hasExactWidth() ? constraints.maxWidth
      : constraints.hasMaxWidth               ? constraints.maxWidth
                                              : 0.0F;
  const float h = constraints.hasExactHeight() ? constraints.maxHeight
      : constraints.hasMaxHeight               ? constraints.maxHeight
                                               : 0.0F;
  return LayoutSize{.width = w, .height = h};
}

void VirtualGridView::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

void VirtualGridView::onScrollChanged(float /*offset*/) {
  if (m_rubberBandPending)
    updateRubberBandGeometry();
  if (animationManager() != nullptr && m_selectionAnimationId != 0) {
    animationManager()->cancel(m_selectionAnimationId);
    m_selectionAnimationId = 0;
  }
  m_selectionIndicatorSnapPending = true;
  if (m_adapterPointerCapture && m_adapter != nullptr) {
    m_adapter->onPointerCancel();
    m_adapterPointerCapture = false;
  }
  if (m_hoveredOverlayIndex.has_value()) {
    setOverlayHoveredForIndex(*m_hoveredOverlayIndex, false);
  }
  m_hoveredIndex.reset();
  m_hoveredOverlayIndex.reset();
  markLayoutDirty();
}

void VirtualGridView::onPointerEnter(float localX, float localY) { onPointerMotion(localX, localY); }

void VirtualGridView::onPointerMotion(float localX, float localY) {
  const auto idx = indexAt(localX, localY);

  if (m_adapterPointerCapture && m_adapter != nullptr) {
    // A held press only becomes a drag once the pointer has travelled the same
    // distance the rest of the shell requires. Pointers emit motion between
    // press and release (and enter is replayed as motion after a scene-root
    // swap), so without this every click would reach the adapter as a
    // zero-distance drag and never as a click.
    if (!m_dragThresholdPassed) {
      if (std::hypot(localX - m_pressLocalX, localY - m_pressLocalY) < Style::dragStartThreshold * m_scale) {
        return;
      }
      m_dragThresholdPassed = true;
    }
    if (m_adapter->onPointerDrag(idx, localX, localY, m_cellWidth, m_cellHeightResolved)) {
      notifyDataChanged();
    }
    return;
  }

  std::optional<std::size_t> overlayIdx;
  if (idx.has_value() && m_adapter != nullptr) {
    float cellLocalX = 0.0F;
    float cellLocalY = 0.0F;
    cellLocalAt(localX, localY, *idx, cellLocalX, cellLocalY);
    if (m_adapter->overlayHitTest(*idx, cellLocalX, cellLocalY, m_cellWidth, m_cellHeightResolved)) {
      overlayIdx = idx;
    }
  }

  if (idx == m_hoveredIndex && overlayIdx == m_hoveredOverlayIndex) {
    return;
  }

  m_tooltipSuppressed = false;
  if (m_hoveredOverlayIndex.has_value() && m_hoveredOverlayIndex != overlayIdx) {
    setOverlayHoveredForIndex(*m_hoveredOverlayIndex, false);
  }
  if (overlayIdx.has_value() && overlayIdx != m_hoveredOverlayIndex) {
    setOverlayHoveredForIndex(*overlayIdx, true);
  }

  m_hoveredIndex = idx;
  m_hoveredOverlayIndex = overlayIdx;
  markLayoutDirty();
}

void VirtualGridView::onPointerLeave() {
  if (!m_hoveredIndex.has_value() && !m_hoveredOverlayIndex.has_value()) {
    return;
  }
  m_tooltipSuppressed = false;
  if (m_hoveredOverlayIndex.has_value()) {
    setOverlayHoveredForIndex(*m_hoveredOverlayIndex, false);
  }
  m_hoveredIndex.reset();
  m_hoveredOverlayIndex.reset();
  markLayoutDirty();
}

void VirtualGridView::onPoolTooltipMotion(std::size_t slot, float localX, float localY) {
  if (slot >= m_slotBoundIndex.size() || !m_slotBoundIndex[slot].has_value() || m_layoutColumns == 0) {
    return;
  }
  const std::size_t index = *m_slotBoundIndex[slot];
  const auto column = index % m_layoutColumns;
  const auto row = index / m_layoutColumns;
  onPointerMotion(
      static_cast<float>(visualCol(column)) * (m_cellWidth + m_columnGap) + localX,
      static_cast<float>(row) * (m_cellHeightResolved + m_rowGap) + localY
  );
}

void VirtualGridView::onPoolTooltipLeave(std::size_t slot) {
  if (slot >= m_slotBoundIndex.size() || (m_hoveredIndex.has_value() && m_slotBoundIndex[slot] == m_hoveredIndex)) {
    onPointerLeave();
  }
}

void VirtualGridView::onPointerPress(float localX, float localY) {
  const auto idx = indexAt(localX, localY);
  if (!idx.has_value()) {
    return;
  }
  setSelectedIndex(idx, false);
  if (m_adapter == nullptr) {
    return;
  }

  float cellLocalX = 0.0F;
  float cellLocalY = 0.0F;
  cellLocalAt(localX, localY, *idx, cellLocalX, cellLocalY);
  if (m_adapter->onPointerPress(*idx, cellLocalX, cellLocalY, m_cellWidth, m_cellHeightResolved)) {
    m_adapterPointerCapture = true;
    m_pressLocalX = localX;
    m_pressLocalY = localY;
    m_dragThresholdPassed = false;
    m_tooltipSuppressed = true;
    for (std::size_t slot = 0; slot < m_pool.size(); ++slot) {
      if (m_slotBoundIndex[slot] == idx) {
        m_poolTooltipAreas[slot]->requestTooltipRefresh();
      }
    }
    return;
  }
  m_adapter->onActivate(*idx);
}

void VirtualGridView::onPointerRelease(float localX, float localY) {
  if (!m_adapterPointerCapture || m_adapter == nullptr) {
    return;
  }
  m_adapterPointerCapture = false;
  const auto result = m_adapter->onPointerRelease(indexAt(localX, localY));
  if (result.rebind) {
    notifyDataChanged();
  }
  if (result.activateIndex.has_value()) {
    m_adapter->onActivate(*result.activateIndex);
  }
}

void VirtualGridView::onSecondaryPointerPress(float localX, float localY) {
  const auto idx = indexAt(localX, localY);
  if (!idx.has_value()) {
    if (m_adapter != nullptr) {
      float wx = 0.0F;
      float wy = 0.0F;
      Node::absolutePosition(m_inputArea, wx, wy);
      m_adapter->onSecondaryEmptyActivate(wx + localX, wy + localY);
    }
    return;
  }
  if (m_adapter != nullptr) {
    float wx = 0.0F;
    float wy = 0.0F;
    Node::absolutePosition(m_inputArea, wx, wy);
    wx += localX;
    wy += localY;
    m_adapter->onSecondaryActivate(*idx, wx, wy);
  }
}

bool VirtualGridView::absoluteAnchorForIndex(std::size_t index, float& outX, float& outY) const noexcept {
  if (m_inputArea == nullptr
      || m_layoutColumns == 0
      || m_cellWidth <= 0.0F
      || m_cellHeightResolved <= 0.0F
      || index >= m_itemCount) {
    return false;
  }

  // Prefer a live pool tile when the item is currently materialized.
  for (std::size_t slot = 0; slot < m_pool.size(); ++slot) {
    if (!m_slotBoundIndex[slot].has_value() || *m_slotBoundIndex[slot] != index || m_pool[slot] == nullptr) {
      continue;
    }
    Node::absolutePosition(m_pool[slot], outX, outY);
    outX += m_pool[slot]->width() * 0.5F;
    outY += m_pool[slot]->height() * 0.5F;
    return true;
  }

  const auto col = index % m_layoutColumns;
  const auto row = index / m_layoutColumns;
  const float colStride = m_cellWidth + m_columnGap;
  const float rowStride = m_cellHeightResolved + m_rowGap;
  float wx = 0.0F;
  float wy = 0.0F;
  Node::absolutePosition(m_inputArea, wx, wy);
  outX = wx + static_cast<float>(visualCol(col)) * colStride + m_cellWidth * 0.5F;
  outY = wy + static_cast<float>(row) * rowStride + m_cellHeightResolved * 0.5F;
  return true;
}

std::optional<std::size_t> VirtualGridView::indexAtSurface(float x, float y) const noexcept {
  float vx = 0, vy = 0;
  Node::absolutePosition(m_scroll, vx, vy);
  if (x < vx || y < vy || x >= vx + m_scroll->width() || y >= vy + m_scroll->height())
    return std::nullopt;
  Node::absolutePosition(m_inputArea, vx, vy);
  return indexAt(x - vx, y - vy);
}

std::optional<std::size_t> VirtualGridView::indexAt(float localX, float localY) const noexcept {
  if (m_layoutColumns == 0 || m_cellWidth <= 0.0F || m_cellHeightResolved <= 0.0F || m_itemCount == 0) {
    return std::nullopt;
  }
  const float colStride = m_cellWidth + m_columnGap;
  const float rowStride = m_cellHeightResolved + m_rowGap;
  if (localX < 0.0F || localY < 0.0F || colStride <= 0.0F || rowStride <= 0.0F) {
    return std::nullopt;
  }
  const auto colF = localX / colStride;
  const auto rowF = localY / rowStride;
  const auto col = static_cast<std::size_t>(std::floor(colF));
  const auto row = static_cast<std::size_t>(std::floor(rowF));
  if (col >= m_layoutColumns) {
    return std::nullopt;
  }
  // Reject the gutter region between cells.
  const float cellLocalX = localX - static_cast<float>(col) * colStride;
  const float cellLocalY = localY - static_cast<float>(row) * rowStride;
  if (cellLocalX >= m_cellWidth || cellLocalY >= m_cellHeightResolved) {
    return std::nullopt;
  }
  const std::size_t idx = row * m_layoutColumns + visualCol(col);
  if (idx >= m_itemCount) {
    return std::nullopt;
  }
  return idx;
}

void VirtualGridView::cellLocalAt(
    float localX, float localY, std::size_t index, float& cellLocalX, float& cellLocalY
) const noexcept {
  const float colStride = m_cellWidth + m_columnGap;
  const float rowStride = m_cellHeightResolved + m_rowGap;
  const auto col = visualCol(index % m_layoutColumns);
  const auto row = index / m_layoutColumns;
  cellLocalX = localX - static_cast<float>(col) * colStride;
  cellLocalY = localY - static_cast<float>(row) * rowStride;
}

void VirtualGridView::setOverlayHoveredForIndex(std::size_t index, bool hovered) {
  if (m_adapter == nullptr) {
    return;
  }
  for (std::size_t slot = 0; slot < m_pool.size(); ++slot) {
    if (!m_slotBoundIndex[slot].has_value() || *m_slotBoundIndex[slot] != index) {
      continue;
    }
    if (m_slotBoundOverlayHovered[slot] == hovered) {
      return;
    }
    m_adapter->applyOverlayHover(*m_pool[slot], hovered);
    m_slotBoundOverlayHovered[slot] = hovered;
    updateTooltipAnchor(slot, index, hovered);
    m_poolTooltipAreas[slot]->requestTooltipRefresh();
    markPaintDirty();
    return;
  }
}

void VirtualGridView::updateTooltipAnchor(std::size_t slot, std::size_t index, bool overlayHovered) {
  InputArea* area = m_poolTooltipAreas[slot];
  const auto insets = m_adapter->itemTooltipAnchorInsets(index, area->width(), area->height(), overlayHovered);
  if (insets.has_value()) {
    area->setTooltipAnchorInsets(*insets);
  } else {
    area->clearTooltipAnchorInsets();
  }
}

void VirtualGridView::setOnRubberBandSelection(RubberBandSelection callback) {
  m_onRubberBandSelection = std::move(callback);
}

bool VirtualGridView::beginRubberBand(float surfaceX, float surfaceY) {
  float x = 0.0F, y = 0.0F;
  Node::absolutePosition(m_scroll, x, y);
  if (surfaceX < x
      || surfaceX >= x + m_scroll->contentViewportWidth()
      || surfaceY < y
      || surfaceY >= y + m_scroll->contentViewportHeight()
      || indexAtSurface(surfaceX, surfaceY).has_value())
    return false;
  endRubberBand(true);
  Node::absolutePosition(m_inputArea, x, y);
  m_rubberAnchorX = surfaceX - x;
  m_rubberAnchorY = surfaceY - y;
  m_rubberSurfaceX = m_rubberPressSurfaceX = surfaceX;
  m_rubberSurfaceY = m_rubberPressSurfaceY = surfaceY;
  m_rubberBandPending = true;
  m_rubberBandDragging = false;
  m_rubberSelectionPublished = false;
  m_rubberIndices.clear();
  m_tooltipSuppressed = true;
  if (animationManager() != nullptr) {
    // A real-time driver keeps edge scrolling alive with a stationary pointer,
    // including when the user has disabled decorative animations.
    m_rubberScrollAnimation = animationManager()->animateTimer(
        0.0F, 3600000.0F, 3600000.0F, Easing::Linear,
        [this, previous = 0.0F](float elapsed) mutable {
          const float seconds = std::clamp((elapsed - previous) / 1000.0F, 0.0F, 0.05F);
          previous = elapsed;
          advanceRubberBandScroll(seconds);
        },
        {}, this
    );
  }
  return true;
}

void VirtualGridView::updateRubberBand(float surfaceX, float surfaceY) {
  if (!m_rubberBandPending)
    return;
  m_rubberSurfaceX = surfaceX;
  m_rubberSurfaceY = surfaceY;
  if (!m_rubberBandDragging) {
    if (std::hypot(surfaceX - m_rubberPressSurfaceX, surfaceY - m_rubberPressSurfaceY)
        < Style::dragStartThreshold * m_scale)
      return;
    m_rubberBandDragging = true;
  }
  updateRubberBandGeometry();
}

void VirtualGridView::updateRubberBandGeometry() {
  if (!m_rubberBandPending || !m_rubberBandDragging)
    return;
  float x = 0.0F, y = 0.0F;
  Node::absolutePosition(m_inputArea, x, y);
  const float endX = std::clamp(m_rubberSurfaceX - x, 0.0F, m_virtualWidth);
  const float endY =
      std::clamp(m_rubberSurfaceY - y, 0.0F, std::max(m_virtualHeight, m_scroll->contentViewportHeight()));
  const float left = std::min(m_rubberAnchorX, endX), right = std::max(m_rubberAnchorX, endX);
  const float top = std::min(m_rubberAnchorY, endY), bottom = std::max(m_rubberAnchorY, endY);
  m_rubberBand->setPosition(left, top);
  m_rubberBand->setSize(right - left, bottom - top);
  m_rubberBand->setVisible(true);
  std::vector<std::size_t> indices;
  const float stride = m_cellHeightResolved + m_rowGap;
  if (stride > 0.0F && m_layoutColumns > 0 && right > left && bottom > top) {
    const auto firstRow = static_cast<std::size_t>(std::floor(top / stride));
    const auto lastRow = static_cast<std::size_t>(std::floor(bottom / stride));
    // Calculate logical cells, independent of the recycled visible tile pool.
    for (std::size_t row = firstRow; row <= lastRow && row * m_layoutColumns < m_itemCount; ++row) {
      const float cellTop = static_cast<float>(row) * stride;
      if (cellTop >= bottom || cellTop + m_cellHeightResolved <= top)
        continue;
      for (std::size_t col = 0; col < m_layoutColumns; ++col) {
        const auto index = row * m_layoutColumns + col;
        const float cellLeft = static_cast<float>(visualCol(col)) * (m_cellWidth + m_columnGap);
        if (index < m_itemCount && cellLeft < right && cellLeft + m_cellWidth > left)
          indices.push_back(index);
      }
    }
  }
  if (!m_rubberSelectionPublished || indices != m_rubberIndices) {
    m_rubberSelectionPublished = true;
    m_rubberIndices = std::move(indices);
    if (m_onRubberBandSelection)
      m_onRubberBandSelection(m_rubberIndices, false, false);
  }
  markLayoutDirty();
}

void VirtualGridView::advanceRubberBandScroll(float seconds) {
  if (!m_rubberBandPending || !m_rubberBandDragging || seconds <= 0.0F)
    return;
  float x = 0.0F, y = 0.0F;
  Node::absolutePosition(m_scroll, x, y);
  const float edge = std::min(32.0F * m_scale, m_scroll->contentViewportHeight() * 0.25F);
  if (edge <= 0.0F)
    return;
  const float pointerY = m_rubberSurfaceY - y;
  const float height = m_scroll->contentViewportHeight();
  float velocity = 0.0F;
  if (pointerY < edge)
    velocity = -std::clamp((edge - pointerY) / edge, 0.0F, 1.0F);
  else if (pointerY > height - edge)
    velocity = std::clamp((pointerY - height + edge) / edge, 0.0F, 1.0F);
  if (velocity != 0.0F)
    m_scroll->setScrollOffset(m_scroll->scrollOffset() + velocity * 480.0F * m_scale * seconds);
}

void VirtualGridView::endRubberBand(bool cancel) {
  if (!m_rubberBandPending)
    return;
  m_rubberBandPending = false;
  m_rubberBandDragging = false;
  m_rubberBand->setVisible(false);
  if (animationManager() != nullptr && m_rubberScrollAnimation != 0)
    animationManager()->cancel(m_rubberScrollAnimation);
  m_rubberScrollAnimation = 0;
  if (m_onRubberBandSelection)
    m_onRubberBandSelection(m_rubberIndices, true, cancel);
  m_rubberIndices.clear();
  markLayoutDirty();
}
