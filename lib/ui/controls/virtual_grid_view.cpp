#include "ui/controls/virtual_grid_view.h"

#include "render/scene/input_area.h"
#include "ui/controls/box.h"
#include "ui/controls/scroll_view.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <memory>
#include <utility>

namespace {
  constexpr float kAreaScrollBand = 32.0F;
  constexpr float kAreaScrollSpeed = 720.0F;
} // namespace

// Internal canvas that reports a virtual size set externally and never moves
// its children during its own layout pass, VirtualGridView positions pool
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
  inputArea->setOnCancel([this] { cancelPointerGesture(); });
  m_inputArea = static_cast<InputArea*>(m_canvas->addChild(std::move(inputArea)));
  auto selectionBox = std::make_unique<Box>();
  selectionBox->setFill(colorSpecFromRole(ColorRole::Primary, 0.14F));
  selectionBox->setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
  selectionBox->setZIndex(60);
  selectionBox->setHitTestVisible(false);
  selectionBox->setParticipatesInLayout(false);
  selectionBox->setVisible(false);
  m_selectionBox = static_cast<Box*>(m_canvas->addChild(std::move(selectionBox)));
}

VirtualGridView::~VirtualGridView() { stopAreaScroll(); }

void VirtualGridView::bindScrollState(ScrollViewState* state) { m_scroll->bindState(state); }

void VirtualGridView::setAdapter(VirtualGridAdapter* adapter) {
  if (m_adapter == adapter) {
    return;
  }
  cancelPointerGesture();
  VirtualGridAdapter* previousAdapter = m_adapter;
  if (m_decoration != nullptr) {
    m_canvas->removeChild(m_decoration);
    m_decoration = nullptr;
  }
  m_adapter = adapter;
  m_itemCount = 0;
  if (m_adapter != nullptr) {
    auto decoration = m_adapter->createDecoration();
    if (decoration) {
      decoration->setZIndex(-1);
      decoration->setParticipatesInLayout(false);
      decoration->setHitTestVisible(false);
      m_decoration = m_canvas->addChild(std::move(decoration));
    }
  }
  m_adapterPointerCapture = false;
  // Drop the existing pool, tiles were built by the previous adapter's createTile().
  for (Node* tile : m_pool) {
    if (tile != nullptr) {
      if (previousAdapter != nullptr) {
        previousAdapter->setTileVisible(*tile, false);
      }
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
  m_slotBoundSelected.clear();
  m_slotBoundHovered.clear();
  m_slotBoundOverlayHovered.clear();
  m_pendingScrollToIndex = false;
  m_selectedIndex.reset();
  m_hoveredIndex.reset();
  m_hoveredOverlayIndex.reset();
  markLayoutDirty();
}

void VirtualGridView::notifyDataChanged() {
  // Force every visible slot to rebind on the next layout.
  std::ranges::fill(m_slotBoundIndex, std::nullopt);
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

void VirtualGridView::setHorizontalOverflow(float width, float offset) {
  width = std::max(0.0F, width);
  offset = std::max(0.0F, offset);
  if (m_horizontalWidth == width && m_horizontalOffset == offset) {
    return;
  }
  m_horizontalWidth = std::max(0.0F, width);
  m_horizontalOffset = offset;
  cancelPointerGesture();
  m_scroll->setHorizontalOverflow(m_horizontalWidth, offset);
  markLayoutDirty();
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

float VirtualGridView::firstVisibleOffset() const {
  const float stride = m_cellHeightResolved + m_rowGap;
  return stride > 0 ? std::fmod(m_scroll->scrollOffset(), stride) : 0;
}

void VirtualGridView::scrollToIndex(std::size_t index, bool alignToTop, float withinRow) {
  m_pendingScrollWithinRow = std::max(0.0F, withinRow);
  m_pendingScrollToIndex = true;
  m_pendingScrollAlignTop = alignToTop;
  m_pendingScrollIndex = index;
  markLayoutDirty();
}

std::optional<std::size_t> VirtualGridView::firstVisibleIndex() const noexcept {
  const float stride = m_cellHeightResolved + m_rowGap;
  if (m_itemCount == 0 || m_layoutColumns == 0 || stride <= 0 || m_scroll->contentViewportHeight() <= 0) {
    return std::nullopt;
  }
  const auto row = static_cast<std::size_t>(std::floor(m_scroll->scrollOffset() / stride));
  return std::min(row * m_layoutColumns, m_itemCount - 1);
}

void VirtualGridView::setSelectedIndex(std::optional<std::size_t> index, bool scrollToSelection) {
  if (!scrollToSelection) {
    m_pendingScrollToIndex = false;
  } else {
    m_pendingScrollAlignTop = false;
  }
  if (m_selectedIndex == index) {
    if (scrollToSelection && m_selectedIndex.has_value()) {
      m_pendingScrollToIndex = true;
      m_pendingScrollIndex = *m_selectedIndex;
      markLayoutDirty();
    }
    return;
  }
  m_selectedIndex = index;
  if (scrollToSelection && m_selectedIndex.has_value()) {
    m_pendingScrollToIndex = true;
    m_pendingScrollIndex = *m_selectedIndex;
  } else {
    m_pendingScrollToIndex = false;
    m_pendingScrollIndex = 0;
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
    if (m_canvas != nullptr) {
      m_canvas->setVirtualSize(0, 0);
      m_inputArea->setFrameSize(0, 0);
    }
    Flex::doLayout(renderer);
    return;
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

  // The horizontal extent changes cell geometry, not the vertical viewport or
  // its scrollbar. Input coordinates remain relative to the translated canvas.
  viewportW = std::max(viewportW, m_horizontalWidth);
  metrics = resolveMetrics(viewportW);
  const std::size_t previousLayoutColumns = m_layoutColumns;
  const std::size_t columns = metrics.columns;
  const float cellW = metrics.cellW;
  const float cellH = metrics.cellH;
  const std::size_t rowCount = metrics.rowCount;
  const float virtualHeight = metrics.virtualHeight;

  if (m_areaPointerCapture
      && (!m_adapter->areaSelectionCurrent()
          || previousLayoutColumns != columns
          || m_cellWidth != cellW
          || m_cellHeightResolved != cellH)) {
    cancelPointerGesture();
  }

  m_layoutColumns = columns;
  m_cellWidth = cellW;
  m_cellHeightResolved = cellH;
  m_virtualWidth = viewportW;
  m_virtualHeight = virtualHeight;

  m_canvas->setVirtualSize(viewportW, std::max(virtualHeight, viewportH));

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
      if (m_pendingScrollAlignTop || rowTop < visibleTop) {
        m_scroll->requestScrollToOffset(rowTop + (m_pendingScrollAlignTop ? m_pendingScrollWithinRow : 0));
      } else if (rowBottom > visibleBottom) {
        m_scroll->requestScrollToOffset(rowBottom - viewportH);
      }
    }
  }

  // Resolve pending jumps and clamp the offset against the new canvas before binding tiles.
  const float areaDelta = std::exchange(m_areaScrollDelta, 0.0F);
  if (m_areaPointerCapture && areaDelta != 0.0F) {
    m_applyingAreaScroll = true;
    m_scroll->requestScrollToOffset(m_scroll->scrollOffset() + areaDelta);
  }
  const float dragDelta = std::exchange(m_externalDragScrollDelta, 0.0F);
  if (m_adapterPointerCapture && m_dragThresholdPassed && dragDelta != 0) {
    m_applyingExternalDragScroll = true;
    m_scroll->requestScrollToOffset(m_scroll->scrollOffset() + dragDelta);
  }
  Flex::doLayout(renderer);
  m_applyingAreaScroll = false;
  m_applyingExternalDragScroll = false;

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
  // same logical index while they remain in the visible window, avoids the
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
        tile->setPosition(x, y);
        tile->setSize(cellW, cellH);
        m_adapter->setTileVisible(*tile, y + cellH > scrollY && y < scrollY + viewportH);

        InputArea* tooltipArea = m_poolTooltipAreas[slot];
        tooltipArea->setPosition(x, y);
        tooltipArea->setFrameSize(cellW, cellH);
        tooltipArea->setVisible(true);
        const bool selected = m_selectedIndex.has_value() && *m_selectedIndex == logicalIndex;
        const bool hovered = m_hoveredIndex.has_value() && *m_hoveredIndex == logicalIndex;
        const bool overlayHovered = m_hoveredOverlayIndex.has_value() && *m_hoveredOverlayIndex == logicalIndex;
        updateTooltipAnchor(slot, logicalIndex, overlayHovered);
        const bool dirty = previousLayoutColumns != columns
            || !m_slotBoundIndex[slot].has_value()
            || *m_slotBoundIndex[slot] != logicalIndex
            || m_slotBoundSelected[slot] != selected
            || m_slotBoundHovered[slot] != hovered;
        if (dirty) {
          m_adapter->bindTile(*tile, logicalIndex, selected, hovered);
          m_slotBoundIndex[slot] = logicalIndex;
          m_slotBoundSelected[slot] = selected;
          m_slotBoundHovered[slot] = hovered;
        }
        if (m_slotBoundOverlayHovered[slot] != overlayHovered) {
          m_adapter->applyOverlayHover(*tile, overlayHovered);
          m_slotBoundOverlayHovered[slot] = overlayHovered;
        }
        tile->setVisible(true);
        // Canvas's doLayout is a no-op, so we lay out each pool tile explicitly
        // here, otherwise the inner glyph/label children never get measured.
        tile->layout(renderer);
      }
    }
  }

  for (std::size_t slot = 0; slot < m_pool.size(); ++slot) {
    if (!slotActive[slot] && m_pool[slot] != nullptr) {
      if (m_pool[slot]->visible()) {
        m_adapter->setTileVisible(*m_pool[slot], false);
        if (m_slotBoundOverlayHovered[slot]) {
          m_adapter->applyOverlayHover(*m_pool[slot], false);
        }
        m_adapter->releaseTile(*m_pool[slot], renderer);
      }
      m_pool[slot]->setVisible(false);
      m_slotBoundIndex[slot].reset();
      if (slot < m_poolTooltipAreas.size() && m_poolTooltipAreas[slot] != nullptr) {
        m_poolTooltipAreas[slot]->setVisible(false);
      }
      m_slotBoundOverlayHovered[slot] = false;
    }
  }

  if (m_decoration != nullptr) {
    m_decoration->setVisible(visibleRowCount > 0);
    if (visibleRowCount > 0) {
      m_adapter->bindDecoration(
          *m_decoration,
          {firstRow * columns, std::min(desiredPoolSize, m_itemCount - firstRow * columns), columns, cellW, cellH,
           m_columnGap, m_rowGap, Style::rtl()}
      );
      m_decoration->arrange(
          renderer,
          {0, static_cast<float>(firstRow) * rowStride, viewportW,
           static_cast<float>(visibleRowCount) * rowStride - m_rowGap}
      );
    }
  }

  // Step 5: position the input overlay across the entire virtual canvas.
  m_inputArea->setPosition(0.0F, 0.0F);
  m_inputArea->setFrameSize(viewportW, std::max(virtualHeight, viewportH));
  m_selectionBox->setVisible(m_areaPointerCapture && m_dragThresholdPassed);
  if (m_selectionBox->visible()) {
    m_selectionBox->arrange(renderer, m_selectionRect);
  }
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

void VirtualGridView::onScrollChanged(float offset) {
  if (m_applyingAreaScroll && m_areaPointerCapture && m_adapter != nullptr && m_adapter->areaSelectionCurrent()) {
    updateAreaSelection(m_areaViewportX, offset + m_areaViewportY);
    updateAreaScroll();
  } else if (!m_applyingExternalDragScroll) {
    cancelPointerGesture();
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
    if (m_areaPointerCapture) {
      m_areaViewportX = localX;
      m_areaViewportY = localY - m_scroll->scrollOffset();
      updateAreaSelection(localX, localY);
      updateAreaScroll();
      return;
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
  m_externalDragScrollDelta = 0;
  const auto idx = indexAt(localX, localY);
  if (!idx.has_value()) {
    cancelPointerGesture();
    if (m_adapter != nullptr && m_adapter->onBackgroundPointerPress()) {
      m_adapterPointerCapture = true;
      m_areaPointerCapture = true;
      m_pressLocalX = localX;
      m_pressLocalY = localY;
      m_dragThresholdPassed = false;
      m_tooltipSuppressed = true;
      notifyDataChanged();
    }
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
  m_externalDragScrollDelta = 0;
  stopAreaScroll();
  if (!m_adapterPointerCapture || m_adapter == nullptr) {
    return;
  }
  if (m_areaPointerCapture && m_dragThresholdPassed) {
    updateAreaSelection(localX, localY);
    if (!m_adapterPointerCapture) {
      return;
    }
  }
  const bool area = std::exchange(m_areaPointerCapture, false);
  m_adapterPointerCapture = false;
  m_selectionBox->setVisible(false);
  const auto result = m_adapter->onPointerRelease(area ? std::nullopt : indexAt(localX, localY));
  markLayoutDirty();
  if (result.rebind) {
    notifyDataChanged();
  }
  if (result.activateIndex.has_value()) {
    m_adapter->onActivate(*result.activateIndex);
  }
}

void VirtualGridView::cancelPointerGesture() {
  m_externalDragScrollDelta = 0;
  stopAreaScroll();
  const bool captured = std::exchange(m_adapterPointerCapture, false);
  m_areaPointerCapture = false;
  if (m_selectionBox != nullptr) {
    m_selectionBox->setVisible(false);
  }
  if (captured && m_adapter != nullptr) {
    m_adapter->onPointerCancel();
  }
}

void VirtualGridView::stopAreaScroll() {
  m_areaScrollTimer.stop();
  m_areaScrollSpeed = 0.0F;
  m_areaScrollDelta = 0.0F;
}

void VirtualGridView::updateAreaScroll() {
  if (!m_areaPointerCapture || !m_dragThresholdPassed || m_adapter == nullptr || !m_adapter->areaSelectionCurrent()) {
    stopAreaScroll();
    return;
  }
  const float height = m_scroll->contentViewportHeight();
  const float band = std::min(kAreaScrollBand, height * 0.25F);
  float speed = 0.0F;
  float sceneX = 0, sceneY = 0;
  Node::mapToScene(m_inputArea, m_areaViewportX, m_scroll->scrollOffset() + m_areaViewportY, sceneX, sceneY);
  const auto viewport = m_scroll->contentViewportRect();
  if (band > 0.0F && sceneX >= viewport.x && sceneX <= viewport.x + viewport.width) {
    if (m_areaViewportY < band) {
      speed = -kAreaScrollSpeed * std::clamp((band - m_areaViewportY) / band, 0.0F, 1.0F);
    } else if (m_areaViewportY > height - band) {
      speed = kAreaScrollSpeed * std::clamp((m_areaViewportY - height + band) / band, 0.0F, 1.0F);
    }
  }
  const float offset = m_scroll->scrollOffset();
  if ((speed < 0.0F && offset <= 0.0F) || (speed > 0.0F && offset >= m_scroll->maxScrollOffset())) {
    speed = 0.0F;
  }
  if (speed == 0.0F) {
    stopAreaScroll();
    return;
  }
  m_areaScrollSpeed = speed;
  if (!m_areaScrollTimer.active()) {
    m_areaScrollTick = std::chrono::steady_clock::now();
    m_areaScrollTimer.startRepeating(std::chrono::milliseconds(16), [this] { tickAreaScroll(); });
  }
}

void VirtualGridView::tickAreaScroll() {
  if (!m_areaPointerCapture || m_adapter == nullptr || !m_adapter->areaSelectionCurrent()) {
    cancelPointerGesture();
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  const float elapsed = std::min(std::chrono::duration<float>(now - m_areaScrollTick).count(), 0.05F);
  m_areaScrollTick = now;
  // Bound accumulated work if the compositor delays the next layout frame.
  m_areaScrollDelta = std::clamp(m_areaScrollDelta + m_areaScrollSpeed * elapsed, -36.0F, 36.0F);
  markLayoutDirty();
}

void VirtualGridView::updateAreaSelection(float localX, float localY) {
  localX = std::clamp(localX, 0.0F, m_virtualWidth);
  localY = std::clamp(localY, m_scroll->scrollOffset(), m_scroll->scrollOffset() + m_scroll->contentViewportHeight());
  m_selectionRect = {
      std::min(localX, m_pressLocalX), std::min(localY, m_pressLocalY), std::abs(localX - m_pressLocalX),
      std::abs(localY - m_pressLocalY)
  };
  const float right = m_selectionRect.x + m_selectionRect.width;
  const float bottom = m_selectionRect.y + m_selectionRect.height;
  std::vector<std::size_t> indices;
  if (m_itemCount > 0 && m_layoutColumns > 0 && m_cellWidth > 0 && m_cellHeightResolved > 0) {
    const float strideX = m_cellWidth + m_columnGap, strideY = m_cellHeightResolved + m_rowGap;
    const auto firstRow = static_cast<std::size_t>(m_selectionRect.y / strideY);
    const auto lastRow = std::min(static_cast<std::size_t>(bottom / strideY), (m_itemCount - 1) / m_layoutColumns);
    const auto firstCol = static_cast<std::size_t>(m_selectionRect.x / strideX);
    const auto lastCol = std::min(static_cast<std::size_t>(right / strideX), m_layoutColumns - 1);
    for (std::size_t row = firstRow; row <= lastRow; ++row) {
      for (std::size_t col = firstCol; col <= lastCol; ++col) {
        const auto index = row * m_layoutColumns + visualCol(col);
        if (index < m_itemCount
            && right > static_cast<float>(col) * strideX
            && m_selectionRect.x < static_cast<float>(col) * strideX + m_cellWidth
            && bottom > static_cast<float>(row) * strideY
            && m_selectionRect.y < static_cast<float>(row) * strideY + m_cellHeightResolved) {
          indices.push_back(index);
        }
      }
    }
  }
  if (!m_adapter->onAreaSelection(indices)) {
    cancelPointerGesture();
  }
  notifyDataChanged();
}

void VirtualGridView::onSecondaryPointerPress(float localX, float localY) {
  cancelPointerGesture();
  const auto idx = indexAt(localX, localY);
  if (!idx.has_value()) {
    if (m_adapter != nullptr) {
      float x = 0, y = 0;
      Node::mapToScene(m_inputArea, localX, localY, x, y);
      m_adapter->onBackgroundSecondaryActivate(x, y);
    }
    return;
  }
  setSelectedIndex(idx, false);
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

  const auto col = visualCol(index % m_layoutColumns);
  const auto row = index / m_layoutColumns;
  Node::mapToScene(
      m_inputArea, static_cast<float>(col) * (m_cellWidth + m_columnGap) + m_cellWidth * 0.5F,
      static_cast<float>(row) * (m_cellHeightResolved + m_rowGap) + m_cellHeightResolved * 0.5F, outX, outY
  );
  return true;
}

std::optional<std::size_t> VirtualGridView::itemAtScene(float x, float y) const noexcept {
  const auto viewport = m_scroll->contentViewportRect();
  if (x < viewport.x || x >= viewport.x + viewport.width || y < viewport.y || y >= viewport.y + viewport.height) {
    return std::nullopt;
  }
  float localX = 0, localY = 0;
  if (!Node::mapFromScene(m_inputArea, x, y, localX, localY)) {
    return std::nullopt;
  }
  return indexAt(localX, localY, false);
}

std::optional<LayoutRect> VirtualGridView::itemRect(std::size_t index) const noexcept {
  float centerX = 0, centerY = 0;
  if (!absoluteAnchorForIndex(index, centerX, centerY)) {
    return std::nullopt;
  }
  const auto col = visualCol(index % m_layoutColumns);
  const auto row = index / m_layoutColumns;
  const float x = static_cast<float>(col) * (m_cellWidth + m_columnGap);
  const float y = static_cast<float>(row) * (m_cellHeightResolved + m_rowGap);
  float left = 0, top = 0, right = 0, bottom = 0;
  Node::mapToScene(m_inputArea, x, y, left, top);
  Node::mapToScene(m_inputArea, x + m_cellWidth, y + m_cellHeightResolved, right, bottom);
  const auto viewport = m_scroll->contentViewportRect();
  left = std::max(left, viewport.x);
  top = std::max(top, viewport.y);
  right = std::min(right, viewport.x + viewport.width);
  bottom = std::min(bottom, viewport.y + viewport.height);
  if (right <= left || bottom <= top) {
    return std::nullopt;
  }
  return LayoutRect{left, top, right - left, bottom - top};
}

std::optional<std::size_t> VirtualGridView::indexAt(float localX, float localY, bool contentOnly) const noexcept {
  if (m_layoutColumns == 0 || m_cellWidth <= 0.0F || m_cellHeightResolved <= 0.0F || m_itemCount == 0) {
    return std::nullopt;
  }
  const float colStride = m_cellWidth + m_columnGap;
  const float rowStride = m_cellHeightResolved + m_rowGap;
  if (localX < 0.0F
      || localY < 0.0F
      || localY >= m_virtualHeight
      || !std::isfinite(localX)
      || !std::isfinite(localY)
      || colStride <= 0.0F
      || rowStride <= 0.0F) {
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
  if (contentOnly
      && m_adapter != nullptr
      && !m_adapter->itemHitTest(idx, cellLocalX, cellLocalY, m_cellWidth, m_cellHeightResolved)) {
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
