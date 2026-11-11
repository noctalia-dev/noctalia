#include "core/files/resource_paths.h"
#include "core/timer_manager.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/text/cairo_glyph_renderer.h"
#include "render/text/cairo_text_renderer.h"
#include "tests/test_check.h"
#include "ui/controls/box.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <functional>
#include <linux/input-event-codes.h>
#include <map>
#include <memory>
#include <print>
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
      TEST_CHECK(index < count);
      ++bound;
    }
    void setTileVisible(Node& tile, bool value) override { visible[&tile] = value; }
    void releaseTile(Node& tile, Renderer&) override {
      TEST_CHECK(!visible[&tile]);
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
    TEST_CHECK(adapter.created < 40 && adapter.created > 0);
    TEST_CHECK(adapter.decorationCount == 1 && adapter.decoration.itemCount <= adapter.created);
    TEST_CHECK(std::ranges::any_of(adapter.visible, [](const auto& pair) { return !pair.second; }));
    const auto created = adapter.created;
    const auto bound = adapter.bound;
    layout(grid, renderer);
    TEST_CHECK(adapter.bound == bound);
    grid.scrollToIndex(90, true, 7);
    layout(grid, renderer);
    TEST_CHECK(grid.firstVisibleIndex() == 90);
    TEST_NEAR(grid.firstVisibleOffset(), 7, 0.1F);
    TEST_CHECK(adapter.created <= created + 6 && adapter.created < 40);
    const auto fullPool = adapter.created;
    grid.scrollToIndex(180, true, 7);
    layout(grid, renderer);
    TEST_CHECK(adapter.created == fullPool);
    grid.scrollToIndex(90, true, 7);
    layout(grid, renderer);
    TEST_CHECK(
        adapter.decoration.firstIndex <= 90 && adapter.decoration.firstIndex + adapter.decoration.itemCount > 90
    );
    for (bool rtl : {false, true}) {
      Style::setRtl(rtl);
      layout(grid, renderer);
      const auto rect = grid.itemRect(94);
      TEST_CHECK(rect.has_value());
      TEST_CHECK(grid.itemAtScene(rect->x + rect->width * 0.5F, rect->y + rect->height * 0.5F) == 94);
      const auto viewport = grid.scrollView().contentViewportRect();
      TEST_CHECK(!grid.itemAtScene(viewport.x + viewport.width + 1, viewport.y + 20));
      TEST_CHECK(!grid.itemRect(0));
      grid.setHorizontalOverflow(700, 150);
      layout(grid, renderer);
      const auto wide = grid.itemRect(94);
      TEST_CHECK(wide && grid.itemAtScene(wide->x + wide->width / 2, wide->y + wide->height / 2) == 94);
      grid.setHorizontalOverflow(0, 0);
      layout(grid, renderer);
    }
    Style::setRtl(false);
    adapter.count = 2;
    grid.notifyDataChanged();
    layout(grid, renderer);
    TEST_CHECK(adapter.released > 0);
    const auto released = adapter.released;
    layout(grid, renderer);
    TEST_CHECK(adapter.released == released);
    TEST_CHECK(adapter.decoration.itemCount == 2);
    Adapter replacement;
    replacement.count = 1;
    grid.setAdapter(&replacement);
    TEST_CHECK(!grid.firstVisibleIndex());
    TEST_CHECK(std::ranges::none_of(adapter.visible, [](const auto& pair) { return pair.second; }));
    layout(grid, renderer);
    grid.setAdapter(nullptr);
    layout(grid, renderer);
    TEST_CHECK(!grid.firstVisibleIndex() && !grid.itemRect(0));
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
    TEST_CHECK(rect && grid.itemAtScene(rect->x + rect->width - 2, rect->y + 10) == 0);
    input->dispatchPress(150, 10, BTN_LEFT, true);
    input->dispatchMotion(5, 75);
    TEST_CHECK(!adapter.selected.empty());
    input->dispatchPress(5, 75, BTN_LEFT, false);
    TEST_CHECK(adapter.activations == 0);
    input->dispatchPress(150, 10, BTN_LEFT, true);
    input->dispatchMotion(150, 195);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    TimerManager::instance().tick();
    layout(grid, renderer);
    TEST_CHECK(grid.scrollView().scrollOffset() > 0);
    TEST_CHECK(adapter.cancelled == 0);
    adapter.onCancel = [&grid] { grid.cancelPointerGesture(); };
    grid.cancelPointerGesture();
    TEST_CHECK(adapter.cancelled == 1);
    const auto offset = grid.scrollView().scrollOffset();
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    TimerManager::instance().tick();
    layout(grid, renderer);
    TEST_NEAR(grid.scrollView().scrollOffset(), offset, 0.1F);
    adapter.onCancel = {};
    grid.scrollToIndex(0, true);
    layout(grid, renderer);
    input->dispatchPress(10, 10, BTN_LEFT, true);
    input->dispatchMotion(10, 50);
    grid.scrollDuringDrag(40);
    layout(grid, renderer);
    TEST_NEAR(grid.scrollView().scrollOffset(), 40, 0.1F);
    TEST_CHECK(adapter.cancelled == 1);
    grid.cancelPointerGesture();
    grid.scrollToIndex(0, true);
    layout(grid, renderer);
    input->dispatchPress(150, 10, BTN_LEFT, true);
    input->dispatchMotion(150, 100);
    adapter.current = false;
    layout(grid, renderer);
    TEST_CHECK(adapter.cancelled == 3);
    input->dispatchPress(150, 100, BTN_LEFT, false);
    TEST_CHECK(adapter.activations == 0);
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
    TEST_CHECK(scroll.scrollOffset() > 0);
    const auto viewport = scroll.contentViewportRect();
    float x = 0, y = 0;
    Node::absolutePosition(tiles.back(), x, y);
    TEST_CHECK(x >= viewport.x - 0.1F && x + 80 <= viewport.x + viewport.width + 0.1F);
    scroll.requestScrollIntoView(tiles.front());
    scroll.arrange(renderer, {.width = 200, .height = 60});
    TEST_NEAR(scroll.scrollOffset(), 0, 0.1F);
    scroll.requestScrollIntoView(tiles.back());
    scroll.content()->removeChild(tiles.back());
    scroll.arrange(renderer, {.width = 200, .height = 60});
    TEST_NEAR(scroll.scrollOffset(), 0, 0.1F);
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
