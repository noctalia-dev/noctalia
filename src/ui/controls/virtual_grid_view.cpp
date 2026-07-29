#include "ui/controls/virtual_grid_view.h"

#include "render/scene/input_area.h"
#include "ui/controls/scroll_view.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <memory>

// Internal canvas that reports a virtual size set externally and never moves
// its children during its own layout pass — VirtualGridView positions pool
// tiles and the overlay InputArea explicitly.
class VirtualGridView::Canvas : public Node {
public:
  Canvas() = default;

  void setVirtualSize(float width, float height) {
    m_virtualWidth = std::max(0.0f, width);
    m_virtualHeight = std::max(0.0f, height);
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
  float m_virtualWidth = 0.0f;
  float m_virtualHeight = 0.0f;
};

VirtualGridView::VirtualGridView() {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(0.0f);
  setPadding(0.0f);
  setFillWidth(true);
  setFillHeight(true);

  auto scroll = std::make_unique<ScrollView>();
  scroll->setFlexGrow(1.0f);
  scroll->setViewportPaddingH(0.0f);
  scroll->setViewportPaddingV(0.0f);
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
  inputArea->setOnLeave([this]() { onPointerLeave(); });
  inputArea->setOnPress([this](const InputArea::PointerData& data) {
    if (!data.pressed) {
      return;
    }
    if (data.button == BTN_LEFT) {
      onPointerPress(data.localX, data.localY);
    } else if (data.button == BTN_RIGHT) {
      onSecondaryPointerPress(data.localX, data.localY);
    }
  });
  m_inputArea = static_cast<InputArea*>(m_canvas->addChild(std::move(inputArea)));
}

void VirtualGridView::setAdapter(VirtualGridAdapter* adapter) {
  if (m_adapter == adapter) {
    return;
  }
  m_adapter = adapter;
  // Drop the existing pool — tiles were built by the previous adapter's createTile().
  for (Node* tile : m_pool) {
    if (tile != nullptr) {
      m_canvas->removeChild(tile);
    }
  }
  m_pool.clear();
  m_slotBoundIndex.clear();
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

void VirtualGridView::setColumns(std::size_t columns) {
  if (m_columns == columns) {
    return;
  }
  m_columns = columns;
  notifyDataChanged();
}

void VirtualGridView::setMinCellWidth(float width) {
  m_minCellWidth = std::max(1.0f, width);
  notifyDataChanged();
}

void VirtualGridView::setCellHeight(float height) {
  m_cellHeight = std::max(1.0f, height);
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
  m_columnGap = std::max(0.0f, gap);
  notifyDataChanged();
}

void VirtualGridView::setRowGap(float gap) {
  m_rowGap = std::max(0.0f, gap);
  notifyDataChanged();
}

void VirtualGridView::setOverscanRows(std::size_t rows) {
  m_overscanRows = rows;
  markLayoutDirty();
}

void VirtualGridView::scrollToIndex(std::size_t index) {
  m_pendingScrollToIndex = true;
  m_pendingScrollIndex = index;
  markLayoutDirty();
}

void VirtualGridView::setSelectedIndex(std::optional<std::size_t> index) {
  if (m_selectedIndex == index) {
    if (m_selectedIndex.has_value()) {
      m_pendingScrollToIndex = true;
      m_pendingScrollIndex = *m_selectedIndex;
      markLayoutDirty();
    }
    return;
  }
  m_selectedIndex = index;
  if (m_selectedIndex.has_value()) {
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
  if (m_scroll == nullptr || m_layoutColumns == 0 || m_cellHeightResolved <= 0.0f) {
    return 1;
  }
  const float viewportH = m_scroll->contentViewportHeight();
  const float rowStride = m_cellHeightResolved + m_rowGap;
  if (rowStride <= 0.0f) {
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

  // Step 1: estimate the canvas's virtual size so ScrollView can measure
  // content height correctly. Match ScrollView's gutter behavior: reserve
  // space for the scrollbar only when the content overflows vertically.
  const float ourW = std::max(0.0f, width());
  const float ourH = std::max(0.0f, height());
  const float padH = m_scroll->viewportPaddingH();
  const float padV = m_scroll->viewportPaddingV();
  const float innerW = std::max(0.0f, ourW - 2.0f * padH);
  const float viewportH = std::max(0.0f, ourH - 2.0f * padV);
  const float scrollbarGutter = Style::scrollbarWidth + Style::scrollbarGap;

  m_itemCount = m_adapter->itemCount();

  struct GridMetrics {
    std::size_t columns = 1;
    float cellW = 0.0f;
    float cellH = 0.0f;
    std::size_t rowCount = 0;
    float virtualHeight = 0.0f;
  };

  auto resolveMetrics = [this](float availableW) {
    GridMetrics metrics;
    metrics.columns = m_columns > 0
        ? std::max<std::size_t>(1, m_columns)
        : std::max<std::size_t>(
              1,
              static_cast<std::size_t>(
                  std::floor((availableW + m_columnGap) / std::max(1.0f, m_minCellWidth + m_columnGap))
              )
          );
    const auto columnsF = static_cast<float>(metrics.columns);
    metrics.cellW = std::max(0.0f, (availableW - (columnsF - 1.0f) * m_columnGap) / std::max(1.0f, columnsF));
    metrics.cellH = m_squareCells ? metrics.cellW : m_cellHeight;
    metrics.rowCount = (m_itemCount + metrics.columns - 1) / metrics.columns;
    metrics.virtualHeight = metrics.rowCount == 0
        ? 0.0f
        : (static_cast<float>(metrics.rowCount) * metrics.cellH + static_cast<float>(metrics.rowCount - 1) * m_rowGap);
    return metrics;
  };

  float viewportW = innerW;
  GridMetrics metrics = resolveMetrics(viewportW);
  if (metrics.virtualHeight > viewportH + 0.5f) {
    viewportW = std::max(0.0f, innerW - scrollbarGutter);
    metrics = resolveMetrics(viewportW);
  }

  const std::size_t previousLayoutColumns = m_layoutColumns;
  const std::size_t columns = metrics.columns;
  const float cellW = metrics.cellW;
  const float cellH = metrics.cellH;
  const std::size_t rowCount = metrics.rowCount;
  const float virtualHeight = metrics.virtualHeight;

  m_layoutColumns = columns;
  m_cellWidth = cellW;
  m_cellHeightResolved = cellH;
  m_virtualWidth = viewportW;
  m_virtualHeight = virtualHeight;

  m_canvas->setVirtualSize(viewportW, virtualHeight);

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
        m_scroll->setScrollOffset(rowTop);
      } else if (rowBottom > visibleBottom) {
        m_scroll->setScrollOffset(rowBottom - viewportH);
      }
    }
  }

  // Step 2: determine the visible row range from current scroll offset, with overscan.
  const float rowStride = cellH + m_rowGap;
  const float scrollY = m_scroll->scrollOffset();
  std::size_t firstRow = 0;
  std::size_t lastRow = 0;
  if (rowStride > 0.0f && rowCount > 0) {
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

        const float x = static_cast<float>(col) * (cellW + m_columnGap);
        const float y = static_cast<float>(row) * (cellH + m_rowGap);
        tile->setPosition(x, y);
        tile->setSize(cellW, cellH);

        const bool selected = m_selectedIndex.has_value() && *m_selectedIndex == logicalIndex;
        const bool hovered = m_hoveredIndex.has_value() && *m_hoveredIndex == logicalIndex;
        const bool overlayHovered = m_hoveredOverlayIndex.has_value() && *m_hoveredOverlayIndex == logicalIndex;
        const bool dirty = !m_slotBoundIndex[slot].has_value()
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
        // here — otherwise the inner glyph/label children never get measured.
        tile->layout(renderer);
      }
    }
  }

  for (std::size_t slot = 0; slot < m_pool.size(); ++slot) {
    if (!slotActive[slot] && m_pool[slot] != nullptr) {
      m_pool[slot]->setVisible(false);
      m_slotBoundIndex[slot].reset();
      m_slotBoundOverlayHovered[slot] = false;
    }
  }

  // Step 5: position the input overlay across the entire virtual canvas.
  m_inputArea->setPosition(0.0f, 0.0f);
  m_inputArea->setFrameSize(viewportW, virtualHeight);

  // Step 6: run the outer Flex layout so ScrollView lays itself out around
  // the canvas with its now-correct virtual size.
  Flex::doLayout(renderer);
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
                                              : 0.0f;
  const float h = constraints.hasExactHeight() ? constraints.maxHeight
      : constraints.hasMaxHeight               ? constraints.maxHeight
                                               : 0.0f;
  return LayoutSize{.width = w, .height = h};
}

void VirtualGridView::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

void VirtualGridView::onScrollChanged(float /*offset*/) { markLayoutDirty(); }

void VirtualGridView::onPointerEnter(float localX, float localY) { onPointerMotion(localX, localY); }

void VirtualGridView::onPointerMotion(float localX, float localY) {
  const auto idx = indexAt(localX, localY);

  std::optional<std::size_t> overlayIdx;
  if (idx.has_value() && m_adapter != nullptr) {
    float cellLocalX = 0.0f;
    float cellLocalY = 0.0f;
    cellLocalAt(localX, localY, *idx, cellLocalX, cellLocalY);
    if (m_adapter->overlayHitTest(*idx, cellLocalX, cellLocalY, m_cellWidth, m_cellHeightResolved)) {
      overlayIdx = idx;
    }
  }

  if (idx == m_hoveredIndex && overlayIdx == m_hoveredOverlayIndex) {
    return;
  }

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
  if (m_hoveredOverlayIndex.has_value()) {
    setOverlayHoveredForIndex(*m_hoveredOverlayIndex, false);
  }
  m_hoveredIndex.reset();
  m_hoveredOverlayIndex.reset();
  markLayoutDirty();
}

void VirtualGridView::onPointerPress(float localX, float localY) {
  const auto idx = indexAt(localX, localY);
  if (!idx.has_value()) {
    return;
  }
  setSelectedIndex(idx);
  if (m_adapter == nullptr) {
    return;
  }

  const float colStride = m_cellWidth + m_columnGap;
  const float rowStride = m_cellHeightResolved + m_rowGap;
  const auto col = *idx % m_layoutColumns;
  const auto row = *idx / m_layoutColumns;
  const float cellLocalX = localX - static_cast<float>(col) * colStride;
  const float cellLocalY = localY - static_cast<float>(row) * rowStride;
  if (m_adapter->onPointerPress(*idx, cellLocalX, cellLocalY, m_cellWidth, m_cellHeightResolved)) {
    return;
  }
  m_adapter->onActivate(*idx);
}

void VirtualGridView::onSecondaryPointerPress(float localX, float localY) {
  const auto idx = indexAt(localX, localY);
  if (!idx.has_value()) {
    return;
  }
  setSelectedIndex(idx);
  if (m_adapter != nullptr) {
    float wx = 0.0f;
    float wy = 0.0f;
    Node::absolutePosition(m_inputArea, wx, wy);
    wx += localX;
    wy += localY;
    m_adapter->onSecondaryActivate(*idx, wx, wy);
  }
}

std::optional<std::size_t> VirtualGridView::indexAt(float localX, float localY) const noexcept {
  if (m_layoutColumns == 0 || m_cellWidth <= 0.0f || m_cellHeightResolved <= 0.0f || m_itemCount == 0) {
    return std::nullopt;
  }
  const float colStride = m_cellWidth + m_columnGap;
  const float rowStride = m_cellHeightResolved + m_rowGap;
  if (localX < 0.0f || localY < 0.0f || colStride <= 0.0f || rowStride <= 0.0f) {
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
  if (cellLocalX > m_cellWidth || cellLocalY > m_cellHeightResolved) {
    return std::nullopt;
  }
  const std::size_t idx = row * m_layoutColumns + col;
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
  const auto col = index % m_layoutColumns;
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
    markPaintDirty();
    return;
  }
}
