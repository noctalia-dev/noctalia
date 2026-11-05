#include "core/files/resource_paths.h"
#include "core/timer_manager.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/text/cairo_glyph_renderer.h"
#include "render/text/cairo_text_renderer.h"
#include "ui/controls/box.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <linux/input-event-codes.h>
#include <map>
#include <memory>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
  class ViewportRenderer final : public Renderer {
  public:
    ViewportRenderer() {
      m_text.initialize(nullptr, nullptr);
      m_glyph.initialize(paths::libraryAssetPath("fonts/noctalia-tabler.ttf").string(), nullptr, nullptr);
    }

    TextMetrics measureText(
        std::string_view text, float fontSize, const TextFont& font, float maxWidth, int maxLines, TextAlign align,
        TextEllipsize ellipsize, bool useMarkup
    ) override {
      const auto m = m_text.measure(1.0F, text, fontSize, font, maxWidth, maxLines, align, ellipsize, useMarkup);
      return TextMetrics{
          .width = m.width,
          .left = m.left,
          .right = m.right,
          .top = m.top,
          .bottom = m.bottom,
          .lineCount = m.lineCount,
          .alignOffsetX = m.alignOffsetX
      };
    }

    TextMetrics measureFont(float fontSize, const TextFont& font) override {
      const auto m = m_text.measureFont(1.0F, fontSize, font);
      return TextMetrics{.top = m.top, .bottom = m.bottom};
    }

    void measureTextCursorStops(
        std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
        std::vector<float>& outStops, const TextFont& font
    ) override {
      m_text.measureCursorStops(1.0F, text, fontSize, byteOffsets, outStops, font);
    }

    void measureTextCursorStopsWrapped(
        std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets, float maxWidth,
        std::vector<TextCursorStop>& outStops, const TextFont& font
    ) override {
      m_text.measureCursorStopsWrapped(1.0F, text, fontSize, byteOffsets, maxWidth, outStops, font);
    }

    void measureTextCharSpans(
        std::string_view text, float fontSize, const std::vector<std::size_t>& byteOffsets,
        std::vector<TextCharSpan>& outSpans, const TextFont& font
    ) override {
      m_text.measureCharSpans(1.0F, text, fontSize, byteOffsets, outSpans, font);
    }

    TextMetrics measureGlyph(char32_t codepoint, float fontSize) override {
      const auto metrics = m_glyph.measureGlyph(1.0F, codepoint, fontSize);
      return {
          .width = metrics.width,
          .left = metrics.left,
          .right = metrics.right,
          .top = metrics.top,
          .bottom = metrics.bottom
      };
    }
    TextureManager& textureManager() override { std::abort(); }
    float renderScale() const noexcept override { return 1.0F; }

  private:
    CairoTextRenderer m_text;
    CairoGlyphRenderer m_glyph;
  };

  void require(bool condition, const char* message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }
  class Adapter final : public VirtualGridAdapter {
  public:
    std::size_t count = 1000;
    std::size_t created = 0, bound = 0, released = 0, cancelled = 0, activations = 0;
    std::size_t decorationCount = 0;
    bool narrowHit = false, background = false, current = true;
    std::map<Node*, bool> visible;
    std::vector<std::size_t> selected;
    VirtualGridDecorationLayout decoration{};
    std::function<void()> onCancel;
    std::size_t itemCount() const override { return count; }
    std::unique_ptr<Node> createTile() override {
      ++created;
      return std::make_unique<Node>();
    }
    void bindTile(Node&, std::size_t index, bool, bool) override {
      require(index < count, "grid bound an out-of-range item");
      ++bound;
    }
    void setTileVisible(Node& tile, bool value) override { visible[&tile] = value; }
    void releaseTile(Node& tile, Renderer&) override {
      require(!visible[&tile], "retired tile still advertised visible resources");
      ++released;
    }
    std::unique_ptr<Node> createDecoration() override {
      ++decorationCount;
      return std::make_unique<Node>();
    }
    void bindDecoration(Node&, const VirtualGridDecorationLayout& layout) override { decoration = layout; }
    bool itemHitTest(std::size_t, float x, float, float width, float) const override {
      return !narrowHit || x < width * 0.5F;
    }
    bool onBackgroundPointerPress() override { return background; }
    bool onAreaSelection(const std::vector<std::size_t>& indices) override {
      selected = indices;
      return current;
    }
    bool areaSelectionCurrent() const override { return current; }
    bool onPointerPress(std::size_t, float, float, float, float) override { return true; }
    void onPointerCancel() override {
      ++cancelled;
      if (onCancel) {
        onCancel();
      }
    }
    void onActivate(std::size_t) override { ++activations; }
  };

  void near(float actual, float expected, const char* message) { require(std::abs(actual - expected) < 0.1F, message); }

  void layout(VirtualGridView& grid, Renderer& renderer) {
    grid.arrange(renderer, {.x = 20, .y = 30, .width = 340, .height = 200});
  }

  void resourceAndGeometry(Renderer& renderer) {
    Adapter adapter;
    VirtualGridView grid;
    grid.setAdapter(&adapter);
    grid.setColumns(3);
    grid.setSquareCells(false);
    grid.setCellHeight(30);
    grid.setColumnGap(6);
    grid.setRowGap(4);
    grid.setOverscanRows(1);
    layout(grid, renderer);
    require(adapter.created < 40 && adapter.created > 0, "pool grew with item count rather than the viewport");
    require(
        adapter.decorationCount == 1 && adapter.decoration.itemCount <= adapter.created,
        "decoration escaped the pooled range"
    );
    require(
        std::ranges::any_of(adapter.visible, [](const auto& pair) { return !pair.second; }),
        "overscan was reported actually visible"
    );
    const auto created = adapter.created;
    const auto bound = adapter.bound;
    layout(grid, renderer);
    require(adapter.bound == bound, "stable layout needlessly rebound the pool");
    grid.scrollToIndex(90, true, 7);
    layout(grid, renderer);
    require(grid.firstVisibleIndex() == 90, "scroll anchor restored the wrong row");
    near(grid.firstVisibleOffset(), 7, "scroll anchor lost the within-row offset");
    require(
        adapter.created <= created + 6 && adapter.created < 40,
        "scrolling grew beyond the upper overscan and partial row"
    );
    const auto fullPool = adapter.created;
    grid.scrollToIndex(180, true, 7);
    layout(grid, renderer);
    require(adapter.created == fullPool, "scrolling past overscan grew the pool again");
    grid.scrollToIndex(90, true, 7);
    layout(grid, renderer);
    require(
        adapter.decoration.firstIndex <= 90 && adapter.decoration.firstIndex + adapter.decoration.itemCount > 90,
        "decoration missed the visible range"
    );
    for (bool rtl : {false, true}) {
      Style::setRtl(rtl);
      layout(grid, renderer);
      const auto rect = grid.itemRect(94);
      require(rect.has_value(), "visible item has no scene rectangle");
      require(
          grid.itemAtScene(rect->x + rect->width * 0.5F, rect->y + rect->height * 0.5F) == 94,
          "scene geometry did not round-trip its index"
      );
      const auto viewport = grid.scrollView().contentViewportRect();
      require(!grid.itemAtScene(viewport.x + viewport.width + 1, viewport.y + 20), "scrollbar lane hit an item");
      require(!grid.itemRect(0), "offscreen item exposed a nonvisible tooltip rectangle");
      grid.setHorizontalOverflow(700, 150);
      layout(grid, renderer);
      const auto wide = grid.itemRect(94);
      require(
          wide && grid.itemAtScene(wide->x + wide->width / 2, wide->y + wide->height / 2) == 94,
          "horizontal offset broke scene hit testing"
      );
      grid.setHorizontalOverflow(0, 0);
      layout(grid, renderer);
    }
    Style::setRtl(false);
    adapter.count = 2;
    grid.notifyDataChanged();
    layout(grid, renderer);
    require(adapter.released > 0, "inactive pool resources were never retired");
    const auto released = adapter.released;
    layout(grid, renderer);
    require(adapter.released == released, "inactive tiles were retired repeatedly");
    require(adapter.decoration.itemCount == 2, "decoration included nonexistent cells in the last row");
    Adapter replacement;
    replacement.count = 1;
    grid.setAdapter(&replacement);
    require(!grid.firstVisibleIndex(), "adapter switch retained stale geometry");
    require(
        std::ranges::none_of(adapter.visible, [](const auto& pair) { return pair.second; }),
        "retired adapter retained visible work"
    );
    layout(grid, renderer);
    grid.setAdapter(nullptr);
    layout(grid, renderer);
    require(!grid.firstVisibleIndex() && !grid.itemRect(0), "detached grid retained item geometry");
  }

  void areaAndDrag(Renderer& renderer) {
    Adapter adapter;
    adapter.narrowHit = true;
    adapter.background = true;
    VirtualGridView grid;
    grid.setAdapter(&adapter);
    grid.setColumns(2);
    grid.setSquareCells(false);
    grid.setCellHeight(30);
    grid.setRowGap(4);
    layout(grid, renderer);
    auto* input = grid.focusArea();
    // The right half of a full cell is background for pointer selection, but remains a drop target.
    const auto rect = grid.itemRect(0);
    require(
        rect && grid.itemAtScene(rect->x + rect->width - 2, rect->y + 10) == 0,
        "drop targeting used the narrower pointer hit shape"
    );
    input->dispatchPress(150, 10, BTN_LEFT, true);
    input->dispatchMotion(5, 75);
    require(!adapter.selected.empty(), "background drag did not publish area indices");
    input->dispatchPress(5, 75, BTN_LEFT, false);
    require(adapter.activations == 0, "area selection activated an item");
    input->dispatchPress(150, 10, BTN_LEFT, true);
    input->dispatchMotion(150, 195);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    TimerManager::instance().tick();
    layout(grid, renderer);
    require(grid.scrollView().scrollOffset() > 0, "area selection did not autoscroll");
    require(adapter.cancelled == 0, "owned autoscroll cancelled its area gesture");
    adapter.onCancel = [&grid] { grid.cancelPointerGesture(); };
    grid.cancelPointerGesture();
    require(adapter.cancelled == 1, "gesture cancellation was not reentrancy safe");
    const auto offset = grid.scrollView().scrollOffset();
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    TimerManager::instance().tick();
    layout(grid, renderer);
    near(grid.scrollView().scrollOffset(), offset, "cancelled autoscroll kept moving");
    adapter.onCancel = {};
    grid.scrollToIndex(0, true);
    layout(grid, renderer);
    input->dispatchPress(10, 10, BTN_LEFT, true);
    input->dispatchMotion(10, 50);
    grid.scrollDuringDrag(40);
    layout(grid, renderer);
    near(grid.scrollView().scrollOffset(), 40, "external drag scroll was lost");
    require(adapter.cancelled == 1, "external drag scroll cancelled its owner");
    grid.cancelPointerGesture();
    grid.scrollToIndex(0, true);
    layout(grid, renderer);
    input->dispatchPress(150, 10, BTN_LEFT, true);
    input->dispatchMotion(150, 100);
    adapter.current = false;
    layout(grid, renderer);
    require(adapter.cancelled == 3, "obsolete area identities were not cancelled");
    input->dispatchPress(150, 100, BTN_LEFT, false);
    require(adapter.activations == 0, "retired area release activated an item");
  }

  void horizontalReveal(Renderer& renderer) {
    ScrollView scroll;
    scroll.setOrientation(ScrollOrientation::Horizontal);
    scroll.setViewportPaddingH(0);
    scroll.setViewportPaddingV(0);
    scroll.content()->setDirection(FlexDirection::Horizontal);
    scroll.content()->setGap(0);
    std::vector<Node*> tiles;
    for (int i = 0; i < 10; ++i) {
      auto tile = std::make_unique<Box>();
      tile->setSize(80, 30);
      tiles.push_back(scroll.content()->addChild(std::move(tile)));
    }
    scroll.arrange(renderer, {.width = 200, .height = 60});
    scroll.requestScrollIntoView(tiles.back());
    scroll.arrange(renderer, {.width = 200, .height = 60});
    require(scroll.scrollOffset() > 0, "horizontal reveal never scrolled");
    const auto viewport = scroll.contentViewportRect();
    float x = 0, y = 0;
    Node::absolutePosition(tiles.back(), x, y);
    require(x >= viewport.x - 0.1F && x + 80 <= viewport.x + viewport.width + 0.1F, "revealed tab remained clipped");
    scroll.requestScrollIntoView(tiles.front());
    scroll.arrange(renderer, {.width = 200, .height = 60});
    near(scroll.scrollOffset(), 0, "horizontal reveal did not restore the first tab");
    scroll.requestScrollIntoView(tiles.back());
    scroll.content()->removeChild(tiles.back());
    scroll.arrange(renderer, {.width = 200, .height = 60});
    near(scroll.scrollOffset(), 0, "removed reveal target still changed scroll position");
  }
} // namespace

int main() {
  try {
    ViewportRenderer renderer;
    resourceAndGeometry(renderer);
    areaAndDrag(renderer);
    horizontalReveal(renderer);
  } catch (const std::exception& error) {
    Style::setRtl(false);
    std::println(stderr, "virtual_grid_view_contract_test: {}", error.what());
    return 1;
  }
  return 0;
}
