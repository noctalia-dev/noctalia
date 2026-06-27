#pragma once

#include "render/scene/node.h"
#include "ui/controls/flex.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class InputArea;
class ScrollView;

// Adapter that drives a VirtualGridView from an external data source.
//
// The grid only ever materializes a small pool of tiles (sized to the
// visible rows + a small overscan). Tiles are created once via createTile()
// and recycled via bindTile() as the user scrolls or the data changes.
//
// Designed so a future Lua-backed adapter can wrap script callbacks: every
// method takes/returns POD-ish data (indices, plain bools), and createTile()
// would map to a "tile template" callback in script land.
class VirtualGridAdapter {
public:
  virtual ~VirtualGridAdapter() = default;

  // Total number of logical items currently in the data source.
  [[nodiscard]] virtual std::size_t itemCount() const = 0;

  // Build one fresh tile node. Called lazily as the visible window grows.
  // Returned node becomes a child of the grid; do not retain ownership.
  [[nodiscard]] virtual std::unique_ptr<Node> createTile() = 0;

  // Bind item `index` into an existing pool tile. Implementations should
  // mutate controls inside `tile` only; never reparent or rebuild the subtree.
  virtual void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) = 0;

  // Optional: respond to an activation gesture (click). The grid still
  // updates its own selection state and fires onSelectionChanged.
  virtual void onActivate(std::size_t /*index*/) {}

  // Return true when an overlay consumed the press.
  virtual bool onPointerPress(
      std::size_t /*index*/, float /*cellLocalX*/, float /*cellLocalY*/, float /*cellWidth*/, float /*cellHeight*/
  ) {
    return false;
  }

  [[nodiscard]] virtual bool overlayHitTest(
      std::size_t /*index*/, float /*cellLocalX*/, float /*cellLocalY*/, float /*cellWidth*/, float /*cellHeight*/
  ) const {
    return false;
  }

  virtual void applyOverlayHover(Node& /*tile*/, bool /*hovered*/) {}

  // Optional: secondary button press (e.g. context menu). Anchor coordinates are in the panel scene graph
  // (surface-local).
  virtual void onSecondaryActivate(std::size_t /*index*/, float /*anchorX*/, float /*anchorY*/) {}
};

class VirtualGridView : public Flex {
public:
  VirtualGridView();

  [[nodiscard]] InputArea* focusArea() const noexcept { return m_inputArea; }

  // Adapter is non-owning and must outlive the grid.
  void setAdapter(VirtualGridAdapter* adapter);

  // Notify the grid that the adapter's item count or contents changed.
  void notifyDataChanged();
  // Rebind a single tile if it is currently in the visible window.
  void notifyItemChanged(std::size_t index);

  void setColumns(std::size_t columns); // 0 = auto from minCellWidth.
  void setMinCellWidth(float width);    // Used when columns == 0.
  void setCellHeight(float height);     // Required unless squareCells is set.
  void setSquareCells(bool square);     // Cell height tracks cell width.
  void setColumnGap(float gap);
  void setRowGap(float gap);
  void setOverscanRows(std::size_t rows);

  void scrollToIndex(std::size_t index);
  void setSelectedIndex(std::optional<std::size_t> index);
  [[nodiscard]] std::optional<std::size_t> selectedIndex() const noexcept { return m_selectedIndex; }
  // Items to move for a Page Up/Down step (one viewport of rows, at least one item).
  [[nodiscard]] std::size_t pageItemStride() const noexcept;
  // Column count from the most recent layout pass (for keyboard navigation).
  [[nodiscard]] std::size_t layoutColumnCount() const noexcept { return m_layoutColumns; }

  void setOnSelectionChanged(std::function<void(std::optional<std::size_t>)> callback);

  [[nodiscard]] ScrollView& scrollView() noexcept { return *m_scroll; }

protected:
  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

private:
  class Canvas;

  void onScrollChanged(float offset);
  void onPointerEnter(float localX, float localY);
  void onPointerMotion(float localX, float localY);
  void onPointerLeave();
  void onPointerPress(float localX, float localY);
  void onSecondaryPointerPress(float localX, float localY);
  [[nodiscard]] std::optional<std::size_t> indexAt(float localX, float localY) const noexcept;
  void cellLocalAt(float localX, float localY, std::size_t index, float& cellLocalX, float& cellLocalY) const noexcept;
  void setOverlayHoveredForIndex(std::size_t index, bool hovered);

  ScrollView* m_scroll = nullptr;
  Canvas* m_canvas = nullptr;
  InputArea* m_inputArea = nullptr;

  VirtualGridAdapter* m_adapter = nullptr;
  std::vector<Node*> m_pool;
  std::vector<std::optional<std::size_t>> m_slotBoundIndex;
  std::vector<bool> m_slotBoundSelected;
  std::vector<bool> m_slotBoundHovered;
  std::vector<bool> m_slotBoundOverlayHovered;

  std::size_t m_columns = 0;
  float m_minCellWidth = 96.0f;
  float m_cellHeight = 96.0f;
  bool m_squareCells = true;
  float m_columnGap = 4.0f;
  float m_rowGap = 4.0f;
  std::size_t m_overscanRows = 2;

  std::optional<std::size_t> m_selectedIndex;
  std::optional<std::size_t> m_hoveredIndex;
  std::optional<std::size_t> m_hoveredOverlayIndex;
  std::function<void(std::optional<std::size_t>)> m_onSelectionChanged;

  // Most recent layout snapshot — used by hit-testing and scrollToIndex
  // without rerunning measurement.
  std::size_t m_layoutColumns = 1;
  float m_cellWidth = 0.0f;
  float m_cellHeightResolved = 0.0f;
  float m_virtualWidth = 0.0f;
  float m_virtualHeight = 0.0f;
  std::size_t m_visibleStartIndex = 0;
  std::size_t m_itemCount = 0;
  bool m_pendingScrollToIndex = false;
  std::size_t m_pendingScrollIndex = 0;
};
