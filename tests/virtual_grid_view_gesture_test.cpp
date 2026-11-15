// Covers the click-vs-drag split for adapter-consumed presses. Pointers emit
// motion between press and release (and a scene-root swap replays enter as
// motion), so a grid that forwards every motion as a drag turns each click on a
// reorderable item into a zero-distance drag the adapter can never tell apart
// from a real one.

#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/style.h"

#include <chrono>
#include <cstddef>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <print>
#include <string_view>
#include <thread>
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
        std::string_view, float fontSize, const TextFont&, float, int, TextAlign, TextEllipsize, bool
    ) override {
      return TextMetrics{.bottom = fontSize};
    }

    TextMetrics measureFont(float fontSize, const TextFont&) override { return TextMetrics{.bottom = fontSize}; }

    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, const TextFont&
    ) override {}

    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, const TextFont&
    ) override {}

    TextMetrics measureGlyph(char32_t, float) override { return TextMetrics{}; }

    TextureManager& textureManager() override { std::abort(); }
    [[nodiscard]] float renderScale() const noexcept override { return 1.0F; }
  };

  // Stands in for the launcher's pinned-entry adapters: consumes the press so it
  // can tell a reorder drag from an activation on release.
  class RecordingAdapter final : public VirtualGridAdapter {
  public:
    int bindings = 0;
    std::size_t count = 4;
    [[nodiscard]] std::size_t itemCount() const override { return count; }
    [[nodiscard]] std::unique_ptr<Node> createTile() override { return std::make_unique<Node>(); }
    void bindTile(Node&, std::size_t, bool, bool) override { ++bindings; }

    bool onPointerPress(std::size_t index, float, float, float, float) override {
      presses.push_back(index);
      return true;
    }

    bool onPointerDrag(std::optional<std::size_t> index, float, float, float, float) override {
      drags.push_back(index);
      return false;
    }

    PointerReleaseResult onPointerRelease(std::optional<std::size_t> index) override {
      releases.push_back(index);
      return {};
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

  for (const float x : {1.0F, 50.0F, 99.0F}) {
    expect(grid.indexAtSurface(x, 20.0F) == 0, "row hit box did not cover icon, label and trailing padding");
  }
  grid.setRowGap(3.0F);
  grid.layout(renderer);
  expect(!grid.indexAtSurface(50.0F, kCellHeight), "row hit box included its trailing gutter edge");
  grid.setRowGap(0.0F);
  grid.layout(renderer);
  const auto bindings = adapter.bindings;
  grid.setSize(160.0F, 200.0F);
  grid.layout(renderer);
  expect(adapter.bindings > bindings, "resizing a cell did not rebind its content and background geometry");
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

  {
    // File drops use surface-local coordinates, including the grid's origin
    // and the scroll transform. Off-viewport rows must never become targets.
    grid.setPosition(73.0F, 91.0F);
    grid.setSize(100.0F, 80.0F);
    grid.layout(renderer);
    grid.scrollView().setScrollOffset(kCellHeight);
    grid.layout(renderer);
    float x = 0, y = 0;
    expect(grid.absoluteAnchorForIndex(2, x, y), "missing surface anchor");
    expect(grid.indexAtSurface(x, y) == 2, "surface hit test ignored origin or scroll offset");
    expect(!grid.indexAtSurface(72.0F, 100.0F), "hit test accepted outside the viewport");
    expect(grid.absoluteAnchorForIndex(0, x, y), "missing clipped item anchor");
    expect(!grid.indexAtSurface(x, y), "hit test accepted a clipped row");
  }

  {
    // Empty-space selection is opt-in and never reaches item activation/drag.
    RecordingAdapter selectionAdapter;
    VirtualGridView selectionGrid;
    selectionGrid.setAdapter(&selectionAdapter);
    selectionGrid.setColumns(1);
    selectionGrid.setSquareCells(false);
    selectionGrid.setCellHeight(40.0F);
    selectionGrid.setRowGap(0.0F);
    selectionGrid.setSize(100.0F, 200.0F);
    selectionGrid.layout(renderer);
    std::vector<std::size_t> hits;
    bool finished = false, cancelled = false;
    selectionGrid.setOnRubberBandSelection([&](const auto& selected, bool done, bool cancel) {
      hits = selected;
      finished = done;
      cancelled = cancel;
      selectionGrid.notifyDataChanged(); // Host selection refresh must not cancel the gesture.
    });
    expect(!selectionGrid.beginRubberBand(20.0F, 20.0F), "rubber band captured an existing item");
    expect(selectionGrid.beginRubberBand(80.0F, 180.0F), "empty viewport space could not start a rubber band");
    selectionGrid.updateRubberBand(20.0F, 65.0F);
    selectionGrid.layout(renderer);
    expect(selectionGrid.rubberBandActive(), "selection refresh cancelled rubber-band capture");
    expect(hits == std::vector<std::size_t>{1, 2, 3}, "list rectangle did not intersect logical rows");
    selectionGrid.endRubberBand();
    expect(finished && !cancelled && !selectionGrid.rubberBandActive(), "release did not commit selection");
    expect(selectionAdapter.presses.empty() && selectionAdapter.drags.empty(), "rubber band initiated item dragging");
    expect(selectionGrid.beginRubberBand(80.0F, 180.0F), "could not start second gesture");
    selectionGrid.updateRubberBand(20.0F, 20.0F);
    selectionGrid.endRubberBand(true);
    expect(finished && cancelled, "Escape/cancellation did not notify selection owner");
    expect(selectionGrid.beginRubberBand(80.0F, 180.0F), "could not start empty click");
    selectionGrid.endRubberBand();
    expect(hits.empty() && finished && !cancelled, "empty click did not report empty committed selection");

    // A gutter start permits selection across many recycled grid rows. The
    // pointer remains at a fixed surface position when scrolling advances.
    selectionAdapter.count = 10000;
    selectionGrid.setColumns(2);
    selectionGrid.setColumnGap(8.0F);
    selectionGrid.notifyDataChanged();
    selectionGrid.layout(renderer);
    float firstX = 0.0F, firstY = 0.0F, secondX = 0.0F, secondY = 0.0F;
    selectionGrid.absoluteAnchorForIndex(0, firstX, firstY);
    selectionGrid.absoluteAnchorForIndex(1, secondX, secondY);
    const float gutterX = (firstX + secondX) * 0.5F;
    expect(selectionGrid.beginRubberBand(gutterX, 10.0F), "grid gutter did not start rubber-band selection");
    selectionGrid.updateRubberBand(1.0F, 180.0F);
    selectionGrid.scrollView().setScrollOffset(1200.0F);
    selectionGrid.layout(renderer);
    expect(
        hits.size() > 30 && hits.back() >= 60,
        "selection depended on recycled visible tiles or stale scroll coordinates"
    );
    selectionGrid.setCellHeight(60.0F);
    selectionGrid.layout(renderer);
    expect(!selectionGrid.rubberBandActive() && cancelled, "zoom left stale selection geometry");

    selectionGrid.setCellHeight(40.0F);
    selectionGrid.scrollView().setScrollOffset(0.0F);
    selectionGrid.layout(renderer);
    AnimationManager animations;
    selectionGrid.setAnimationManager(&animations);
    expect(selectionGrid.beginRubberBand(gutterX, 10.0F), "could not begin edge-scroll gesture");
    selectionGrid.updateRubberBand(1.0F, 199.0F);
    const auto initialHits = hits.size();
    const auto initialOffset = selectionGrid.scrollView().scrollOffset();
    // The animation driver uses elapsed wall time. No pointer motion is sent
    // after this wait: a held pointer must keep scrolling and selecting.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    animations.tick(60.0F);
    selectionGrid.layout(renderer);
    expect(selectionGrid.scrollView().scrollOffset() > initialOffset, "stationary edge pointer did not auto-scroll");
    expect(hits.size() > initialHits, "auto-scroll did not extend selection with a stationary pointer");
    selectionGrid.endRubberBand(true);
    expect(!animations.hasActive(), "cancel left the edge-scroll driver running");
    selectionGrid.setAnimationManager(nullptr);
  }

  if (gFailures != 0) {
    std::println(stderr, "virtual_grid_view_gesture_test: {} failure(s)", gFailures);
    return 1;
  }
  return 0;
}
