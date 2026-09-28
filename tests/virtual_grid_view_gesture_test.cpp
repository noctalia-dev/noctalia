// Covers the click-vs-drag split for adapter-consumed presses. Pointers emit
// motion between press and release (and a scene-root swap replays enter as
// motion), so a grid that forwards every motion as a drag turns each click on a
// reorderable item into a zero-distance drag the adapter can never tell apart
// from a real one.

#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/style.h"

#include <cstddef>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <print>
#include <string_view>
#include <vector>

namespace {

  int gFailures = 0;

  void expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "virtual_grid_view_gesture_test: {}", message);
      ++gFailures;
    }
  }

  class StubRenderer final : public Renderer {
  public:
    TextMetrics measureText(
        std::string_view, float fontSize, FontWeight, float, int, TextAlign, std::string_view, TextEllipsize, bool
    ) override {
      return TextMetrics{.bottom = fontSize};
    }

    TextMetrics measureFont(float fontSize, FontWeight) override { return TextMetrics{.bottom = fontSize}; }

    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, FontWeight
    ) override {}

    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, FontWeight
    ) override {}

    TextMetrics measureGlyph(char32_t, float) override { return TextMetrics{}; }

    TextureManager& textureManager() override { std::abort(); }
    [[nodiscard]] float renderScale() const noexcept override { return 1.0F; }
  };

  // Stands in for the launcher's pinned-entry adapters: consumes the press so it
  // can tell a reorder drag from an activation on release.
  class RecordingAdapter final : public VirtualGridAdapter {
  public:
    [[nodiscard]] std::size_t itemCount() const override { return 4; }
    [[nodiscard]] std::unique_ptr<Node> createTile() override { return std::make_unique<Node>(); }
    void bindTile(Node&, std::size_t, bool, bool) override {}

    bool onPointerPress(std::size_t index, float, float, float, float) override {
      presses.push_back(index);
      return true;
    }

    bool onPointerDrag(std::optional<std::size_t> index, float, float, float, float) override {
      drags.push_back(index);
      return false;
    }

    bool onPointerRelease(std::optional<std::size_t> index) override {
      releases.push_back(index);
      return false;
    }

    void reset() {
      presses.clear();
      drags.clear();
      releases.clear();
    }

    std::vector<std::size_t> presses;
    std::vector<std::optional<std::size_t>> drags;
    std::vector<std::optional<std::size_t>> releases;
  };

  constexpr float kCellHeight = 40.0F;

} // namespace

int main() {
  StubRenderer renderer;
  RecordingAdapter adapter;

  VirtualGridView grid;
  grid.setAdapter(&adapter);
  grid.setColumns(1);
  grid.setSquareCells(false);
  grid.setCellHeight(kCellHeight);
  grid.setRowGap(0.0F);
  grid.setSize(100.0F, 200.0F);
  grid.layout(renderer);

  InputArea* area = grid.focusArea();
  if (area == nullptr) {
    std::println(stderr, "virtual_grid_view_gesture_test: grid has no input area");
    return 1;
  }

  const float threshold = Style::dragStartThreshold;

  {
    // A click that jitters below the threshold is a click, not a drag.
    adapter.reset();
    area->dispatchPress(10.0F, 10.0F, BTN_LEFT, true);
    area->dispatchMotion(10.0F + threshold - 0.5F, 10.0F);
    area->dispatchPress(10.0F + threshold - 0.5F, 10.0F, BTN_LEFT, false);
    expect(adapter.presses == std::vector<std::size_t>{0}, "press did not reach the adapter");
    expect(adapter.drags.empty(), "sub-threshold motion was forwarded as a drag");
    expect(
        adapter.releases == std::vector<std::optional<std::size_t>>{std::optional<std::size_t>{0}},
        "release did not report the item under the pointer"
    );
  }

  {
    // The motion replayed after a scene-root swap carries the press position.
    adapter.reset();
    area->dispatchPress(10.0F, 10.0F, BTN_LEFT, true);
    area->dispatchMotion(10.0F, 10.0F);
    area->dispatchPress(10.0F, 10.0F, BTN_LEFT, false);
    expect(adapter.drags.empty(), "zero-distance motion was forwarded as a drag");
    expect(adapter.releases.size() == 1, "release was not delivered");
  }

  {
    // Past the threshold the adapter gets the drag, with the item under the
    // pointer, and no drag event for the travel that was still below it.
    adapter.reset();
    area->dispatchPress(10.0F, 10.0F, BTN_LEFT, true);
    area->dispatchMotion(10.0F, 10.0F + threshold - 0.5F);
    area->dispatchMotion(10.0F, 10.0F + threshold + 1.0F);
    area->dispatchMotion(10.0F, kCellHeight + 10.0F);
    area->dispatchPress(10.0F, kCellHeight + 10.0F, BTN_LEFT, false);
    expect(
        adapter.drags
            == std::vector<std::optional<std::size_t>>{
                std::optional<std::size_t>{0},
                std::optional<std::size_t>{1},
            },
        "drag delivery did not start exactly at the threshold crossing"
    );
    expect(
        adapter.releases == std::vector<std::optional<std::size_t>>{std::optional<std::size_t>{1}},
        "release did not report the item the drag ended on"
    );
  }

  {
    // Once armed, the gesture stays a drag even if the pointer returns inside
    // the threshold radius.
    adapter.reset();
    area->dispatchPress(10.0F, 10.0F, BTN_LEFT, true);
    area->dispatchMotion(10.0F, 10.0F + threshold + 1.0F);
    area->dispatchMotion(10.0F, 10.0F);
    area->dispatchPress(10.0F, 10.0F, BTN_LEFT, false);
    expect(
        adapter.drags
            == std::vector<std::optional<std::size_t>>{
                std::optional<std::size_t>{0},
                std::optional<std::size_t>{0},
            },
        "return inside the threshold radius disarmed the drag"
    );
  }

  {
    // The threshold is a logical distance: it scales with the surface.
    adapter.reset();
    grid.setScale(2.0F);
    area->dispatchPress(10.0F, 10.0F, BTN_LEFT, true);
    area->dispatchMotion(10.0F + threshold + 1.0F, 10.0F);
    expect(adapter.drags.empty(), "threshold ignored the content scale");
    area->dispatchMotion(10.0F + 2.0F * threshold + 1.0F, 10.0F);
    expect(adapter.drags.size() == 1, "scaled threshold never armed the drag");
    area->dispatchPress(10.0F + 2.0F * threshold + 1.0F, 10.0F, BTN_LEFT, false);
    grid.setScale(1.0F);
  }

  {
    // A fresh press re-arms: the anchor is the press point, not wherever the
    // previous gesture ended.
    adapter.reset();
    area->dispatchPress(10.0F, 10.0F, BTN_LEFT, true);
    area->dispatchMotion(10.0F, 10.0F + threshold + 1.0F);
    area->dispatchPress(10.0F, 10.0F + threshold + 1.0F, BTN_LEFT, false);
    expect(adapter.drags.size() == 1, "first gesture did not arm");

    adapter.reset();
    area->dispatchPress(10.0F, 10.0F, BTN_LEFT, true);
    area->dispatchMotion(10.0F, 10.0F + threshold - 0.5F);
    area->dispatchPress(10.0F, 10.0F + threshold - 0.5F, BTN_LEFT, false);
    expect(adapter.drags.empty(), "second press reused the previous drag state");
    expect(adapter.releases.size() == 1, "second release was not delivered");
  }

  if (gFailures != 0) {
    std::println(stderr, "virtual_grid_view_gesture_test: {} failure(s)", gFailures);
    return 1;
  }
  return 0;
}
