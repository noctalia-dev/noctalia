#pragma once

#include "render/scene/node.h"
#include "ui/controls/flex.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class Renderer;
class ScrollView;

// Adapter that drives a VirtualListView from an external data source.
//
// The list materializes only the visible items plus a small overscan. Items are
// created once via createItem() and recycled via bindItem() as the user scrolls
// or the data changes. Rows may have variable heights; measureItem() must be
// pure measurement work and should not mutate scene children or upload textures.
class VirtualListAdapter {
public:
  virtual ~VirtualListAdapter() = default;

  [[nodiscard]] virtual std::size_t itemCount() const = 0;
  [[nodiscard]] virtual std::uint64_t itemKey(std::size_t index) const { return static_cast<std::uint64_t>(index); }
  [[nodiscard]] virtual std::uint64_t itemRevision(std::size_t /*index*/) const { return 0; }
  [[nodiscard]] virtual bool itemInteractive(std::size_t /*index*/) const { return false; }

  [[nodiscard]] virtual float measureItem(Renderer& renderer, std::size_t index, float width) = 0;
  [[nodiscard]] virtual std::unique_ptr<Node> createItem() = 0;
  virtual void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool hovered) = 0;
  virtual void onActivate(std::size_t /*index*/) {}
};

class VirtualListView : public Flex {
public:
  VirtualListView();

  // Adapter is non-owning and must outlive the list.
  void setAdapter(VirtualListAdapter* adapter);

  void notifyDataChanged();
  void notifyItemChanged(std::size_t index);

  void setItemGap(float gap);
  void setOverscanItems(std::size_t items);
  void scrollToIndex(std::size_t index);

  [[nodiscard]] ScrollView& scrollView() noexcept { return *m_scroll; }

protected:
  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

private:
  class Canvas;
  class Slot;

  struct HeightCache {
    std::uint64_t key = 0;
    std::uint64_t revision = 0;
    int widthKey = 0;
    float height = 0.0f;
    bool valid = false;
  };

  void onScrollChanged(float offset);
  void setHoveredIndex(std::optional<std::size_t> index);
  void activateSlot(const Slot& slot);
  void recomputeMetrics(Renderer& renderer, float width);
  void clearSlotBindings();
  void clearHeightCache(std::size_t index);
  [[nodiscard]] std::size_t firstVisibleIndex(float scrollY) const noexcept;
  [[nodiscard]] std::size_t visibleEndIndex(std::size_t first, float scrollBottom) const noexcept;

  ScrollView* m_scroll = nullptr;
  Canvas* m_canvas = nullptr;

  VirtualListAdapter* m_adapter = nullptr;
  std::vector<Slot*> m_pool;
  std::vector<std::optional<std::size_t>> m_slotBoundIndex;
  std::vector<std::uint64_t> m_slotBoundKey;
  std::vector<std::uint64_t> m_slotBoundRevision;
  std::vector<int> m_slotBoundWidthKey;
  std::vector<bool> m_slotBoundHovered;

  std::vector<HeightCache> m_heightCache;
  std::vector<float> m_itemHeights;
  std::vector<float> m_itemOffsets;

  float m_itemGap = 0.0f;
  float m_virtualWidth = 0.0f;
  float m_virtualHeight = 0.0f;
  std::size_t m_overscanItems = 3;
  std::size_t m_itemCount = 0;
  std::optional<std::size_t> m_hoveredIndex;
  bool m_pendingScrollToIndex = false;
  std::size_t m_pendingScrollIndex = 0;
};
