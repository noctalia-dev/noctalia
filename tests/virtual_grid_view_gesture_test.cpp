// Covers the click-vs-drag split for adapter-consumed presses. Pointers emit
// motion between press and release (and a scene-root swap replays enter as
// motion), so a grid that forwards every motion as a drag turns each click on a
// reorderable item into a zero-distance drag the adapter can never tell apart
// from a real one.

#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/style.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <print>
#include <string_view>
#include <thread>
#include <vector>
#include <wayland-client-protocol.h>

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

    void onMiddleActivate(std::size_t index) override { middleActivations.push_back(index); }

    void reset() {
      presses.clear();
      drags.clear();
      releases.clear();
      middleActivations.clear();
    }

    std::vector<std::size_t> presses;
    std::vector<std::optional<std::size_t>> drags;
    std::vector<std::optional<std::size_t>> releases;
    std::vector<std::size_t> middleActivations;
  };

  class InlineEditAdapter final : public VirtualGridAdapter {
  public:
    InputArea* editor = nullptr;
    Node* tile = nullptr;
    int presses = 0;
    int motions = 0;
    std::size_t itemCount() const override { return 1; }
    std::unique_ptr<Node> createTile() override {
      auto node = std::make_unique<Node>();
      tile = node.get();
      tile->setZIndex(7);
      auto input = std::make_unique<InputArea>();
      editor = input.get();
      editor->setFocusable(true);
      // Model Input's persistent text-field focus after pointer release.
      editor->setRetainsFocusOnPointerRelease(true);
      editor->setPosition(10, 5);
      editor->setFrameSize(70, 28);
      editor->setVisible(false);
      editor->setOnPress([this](const InputArea::PointerData&) { ++presses; });
      editor->setOnMotion([this](const InputArea::PointerData&) { ++motions; });
      node->addChild(std::move(input));
      return node;
    }
    void bindTile(Node&, std::size_t, bool, bool) override {}
    InputArea* beginInlineEdit(Node&, const std::string&, const std::string&,
        std::function<void(const std::string&)>, std::function<void()>) override {
      editor->setVisible(true);
      return editor;
    }
    void endInlineEdit(Node&, const std::string&) override { editor->setVisible(false); }
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

  {
    InlineEditAdapter inlineAdapter;
    VirtualGridView inlineGrid;
    inlineGrid.setAdapter(&inlineAdapter);
    inlineGrid.setColumns(1);
    inlineGrid.setSquareCells(false);
    inlineGrid.setCellHeight(kCellHeight);
    inlineGrid.setSize(100, 100);
    inlineGrid.layout(renderer);
    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&inlineGrid);
    auto* editor = inlineGrid.beginInlineEdit(0, "/tmp/name", "name", {}, {});
    expect(editor != nullptr, "inline editor did not open");
    dispatcher.pointerEnter(20, 15, 1);
    (void)dispatcher.pointerButton(20, 15, BTN_LEFT, true, 2, 1, false);
    dispatcher.pointerMotion(40, 15, 3);
    (void)dispatcher.pointerButton(40, 15, BTN_LEFT, false, 4, 2, false);
    expect(dispatcher.focusedArea() == editor, "grid overlay stole the inline editor click");
    expect(inlineAdapter.presses == 2, "inline editor did not receive press and release");
    expect(inlineAdapter.motions > 0, "inline editor did not receive text-selection drag motion");
    inlineGrid.endInlineEdit("/tmp/name");
    expect(inlineAdapter.tile->zIndex() == 7, "inline editing changed the tile's original stacking order");
  }

  {
    // Stage 1: ScrollView::bindState stops active animations and isolates userScrollGeneration.
    AnimationManager scrollAnims;
    ScrollViewState stateA{.offset = 0.0F};
    ScrollViewState stateB{.offset = 240.0F};
    ScrollView scroll;
    scroll.setAnimationManager(&scrollAnims);
    scroll.setSize(120.0F, 100.0F);
    scroll.content()->setDirection(FlexDirection::Vertical);
    auto tallChild = std::make_unique<Node>();
    tallChild->setSize(100.0F, 1000.0F);
    scroll.content()->addChild(std::move(tallChild));
    scroll.bindState(&stateA);
    scroll.layout(renderer);

    expect(
        scroll.viewportArea()->dispatchAxis(
            40.0F, 40.0F, WL_POINTER_AXIS_VERTICAL_SCROLL, WL_POINTER_AXIS_SOURCE_WHEEL, 15.0, 1, 120, 1.0F
        ),
        "wheel scroll axis event was rejected"
    );
    expect(scrollAnims.hasActive(), "wheel scroll did not start scroll animation");
    expect(stateA.userScrollGeneration == 1, "wheel scroll did not advance stateA userScrollGeneration");

    scroll.bindState(&stateB);
    expect(!scrollAnims.hasActive(), "bindState did not stop the outgoing scroll animation");
    scroll.layout(renderer);
    scrollAnims.tick(50.0F);
    expect(std::abs(stateB.offset - 240.0F) < 0.01F, "outgoing animation overwrote incoming stateB offset");
    expect(stateB.userScrollGeneration == 0, "stateB inherited stateA userScrollGeneration");
    expect(scroll.userScrollGeneration() == 0, "ScrollView reported stale userScrollGeneration for stateB");

    scroll.bindState(&stateA);
    scroll.layout(renderer);
    expect(scroll.userScrollGeneration() == 1, "re-binding stateA lost its userScrollGeneration");
    scroll.setAnimationManager(nullptr);
  }

  {
    // Stage 1: Horizontal ScrollView::requestScrollIntoView and vertical-to-horizontal wheel mapping.
    ScrollView hScroll;
    hScroll.setOrientation(ScrollOrientation::Horizontal);
    hScroll.setScrollbarVisible(false);
    hScroll.setMapVerticalWheelToHorizontal(true);
    hScroll.setViewportPaddingH(0.0F);
    hScroll.setViewportPaddingV(0.0F);
    hScroll.setSize(200.0F, 32.0F);
    hScroll.content()->setDirection(FlexDirection::Horizontal);
    hScroll.content()->setGap(4.0F);
    std::vector<Node*> children;
    for (int i = 0; i < 5; ++i) {
      auto item = std::make_unique<Node>();
      item->setSize(100.0F, 32.0F);
      children.push_back(hScroll.content()->addChild(std::move(item)));
    }
    hScroll.layout(renderer);
    expect(hScroll.scrollable(), "horizontal ScrollView did not become scrollable");
    expect(hScroll.scrollOffset() == 0.0F, "initial horizontal scrollOffset was not 0");

    hScroll.requestScrollIntoView(children.back());
    hScroll.layout(renderer);
    expect(hScroll.scrollOffset() > 250.0F, "horizontal requestScrollIntoView did not reveal the trailing item");

    hScroll.requestScrollIntoView(children.front());
    hScroll.layout(renderer);
    expect(hScroll.scrollOffset() == 0.0F, "horizontal requestScrollIntoView did not scroll back to leading item");

    expect(
        hScroll.viewportArea()->dispatchAxis(
            40.0F, 16.0F, WL_POINTER_AXIS_VERTICAL_SCROLL, WL_POINTER_AXIS_SOURCE_WHEEL, 15.0, 1, 120, 1.0F
        ),
        "vertical wheel axis event on horizontal ScrollView was rejected"
    );
    expect(hScroll.scrollOffset() > 0.0F, "vertical wheel did not scroll horizontal ScrollView when enabled");
  }

  {
    // Stage 1: VirtualGridView middle-click activation & transient-state swap reset.
    RecordingAdapter middleAdapter;
    VirtualGridView middleGrid;
    middleGrid.setAdapter(&middleAdapter);
    middleGrid.setColumns(1);
    middleGrid.setSquareCells(false);
    middleGrid.setCellHeight(kCellHeight);
    middleGrid.setRowGap(0.0F);
    middleGrid.setSize(100.0F, 200.0F);
    middleGrid.layout(renderer);

    InputDispatcher dispatcher;
    dispatcher.setSceneRoot(&middleGrid);
    dispatcher.pointerEnter(20.0F, 20.0F, 1);
    expect(dispatcher.pointerButton(20.0F, 20.0F, BTN_MIDDLE, true, 2, 1, false), "middle press not accepted");
    expect(dispatcher.pointerButton(20.0F, 20.0F, BTN_MIDDLE, false, 3, 2, false), "middle release not accepted");
    expect(
        middleAdapter.middleActivations == std::vector<std::size_t>{0},
        "middle-click on item 0 did not trigger onMiddleActivate"
    );
    expect(middleAdapter.presses.empty(), "middle-click leaked into left-press adapter handling");

    // Dragging off to another item before release cancels middle activation.
    middleAdapter.reset();
    (void)dispatcher.pointerButton(20.0F, 20.0F, BTN_MIDDLE, true, 4, 3, false);
    dispatcher.pointerMotion(20.0F, kCellHeight + 20.0F, 5);
    (void)dispatcher.pointerButton(20.0F, kCellHeight + 20.0F, BTN_MIDDLE, false, 6, 4, false);
    expect(middleAdapter.middleActivations.empty(), "dragged middle-click still activated");

    // resetTransientStateForDataSwap cancels in-flight middle press.
    (void)dispatcher.pointerButton(20.0F, 20.0F, BTN_MIDDLE, true, 7, 5, false);
    middleGrid.resetTransientStateForDataSwap();
    (void)dispatcher.pointerButton(20.0F, 20.0F, BTN_MIDDLE, false, 8, 6, false);
    expect(middleAdapter.middleActivations.empty(), "resetTransientStateForDataSwap did not cancel middle press");
  }

  if (gFailures != 0) {
    std::println(stderr, "virtual_grid_view_gesture_test: {} failure(s)", gFailures);
    return 1;
  }
  return 0;
}
