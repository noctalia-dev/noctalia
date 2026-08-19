#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"

#include <cstdio>
#include <linux/input-event-codes.h>
#include <memory>
#include <print>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "input_dispatcher_test: {}", message);
      return false;
    }
    return true;
  }

  InputArea* addArea(Node& root) {
    auto area = std::make_unique<InputArea>();
    area->setSize(40.0F, 40.0F);
    return static_cast<InputArea*>(root.addChild(std::move(area)));
  }

  bool press(InputDispatcher& dispatcher) {
    dispatcher.pointerEnter(10.0F, 10.0F, 1);
    return dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, true, 0, 0, false);
  }

  struct TouchScrollScene {
    TouchScrollScene() {
      root->setSize(100.0F, 100.0F);

      auto viewportNode = std::make_unique<InputArea>();
      viewportNode->setSize(100.0F, 100.0F);
      viewportNode->setTouchScrollAxis(InputArea::TouchScrollAxis::Vertical);
      viewport = static_cast<InputArea*>(root->addChild(std::move(viewportNode)));

      auto rowNode = std::make_unique<InputArea>();
      rowNode->setSize(40.0F, 40.0F);
      row = static_cast<InputArea*>(viewport->addChild(std::move(rowNode)));
    }

    std::unique_ptr<Node> root = std::make_unique<Node>();
    InputArea* viewport = nullptr;
    InputArea* row = nullptr;
  };

} // namespace

int main() {
  bool ok = true;

  {
    InputDispatcher dispatcher;
    auto root = std::make_unique<Node>();
    root->setSize(100.0F, 100.0F);
    auto* area = addArea(*root);
    InputArea::PointerData clicked;
    area->setOnClick([&clicked](const InputArea::PointerData& data) { clicked = data; });
    dispatcher.setSceneRoot(root.get());
    dispatcher.pointerEnter(10.0F, 12.0F, 40);
    (void)dispatcher.pointerButton(10.0F, 12.0F, BTN_LEFT, true, 41, 100, false);
    (void)dispatcher.pointerButton(11.0F, 13.0F, BTN_LEFT, false, 42, 101, false);
    ok = expect(
             clicked.sceneX == 10.0F && clicked.sceneY == 12.0F && clicked.serial == 41 && clicked.time == 100,
             "click callbacks preserve the initiating press coordinates, serial, and time"
         )
        && ok;
  }

  {
    InputDispatcher dispatcher;
    auto root = std::make_unique<Node>();
    root->setSize(100.0F, 100.0F);
    auto* area = addArea(*root);
    int cancellations = 0;
    int clicks = 0;
    bool captureVisibleDuringCallback = false;
    bool pressedClearedBeforeCallback = false;
    area->setOnCancel([&]() {
      ++cancellations;
      captureVisibleDuringCallback = dispatcher.pointerCaptured();
      pressedClearedBeforeCallback = !area->pressed();
      dispatcher.cancelPointerCapture();
    });
    area->setOnClick([&clicks](const InputArea::PointerData&) { ++clicks; });
    dispatcher.setSceneRoot(root.get());

    ok = expect(press(dispatcher), "explicit cancellation setup press is consumed") && ok;
    dispatcher.cancelPointerCapture();
    dispatcher.cancelPointerCapture();

    ok = expect(cancellations == 1, "explicit cancellation fires callback once") && ok;
    ok = expect(captureVisibleDuringCallback, "capture remains visible during cancellation callback") && ok;
    ok = expect(pressedClearedBeforeCallback, "pressed state clears before cancellation callback") && ok;
    ok = expect(!dispatcher.pointerCaptured(), "explicit cancellation clears capture") && ok;
    ok = expect(!area->pressed(), "explicit cancellation clears pressed state") && ok;

    dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, false, 0, 0, false);
    ok = expect(clicks == 0, "release after explicit cancellation does not click") && ok;
  }

  {
    InputDispatcher dispatcher;
    auto root = std::make_unique<Node>();
    root->setSize(100.0F, 100.0F);
    auto* area = addArea(*root);
    int cancellations = 0;
    area->setOnCancel([&cancellations]() { ++cancellations; });
    dispatcher.setSceneRoot(root.get());

    ok = expect(press(dispatcher), "armed pointer-leave setup press is consumed") && ok;
    dispatcher.pointerLeave();

    ok = expect(cancellations == 1, "pointer leave cancels armed capture exactly once") && ok;
    ok = expect(!dispatcher.pointerCaptured(), "armed pointer leave clears capture") && ok;
    ok = expect(!area->pressed(), "armed pointer leave clears pressed state") && ok;
  }

  {
    InputDispatcher dispatcher;
    auto root = std::make_unique<Node>();
    root->setSize(100.0F, 100.0F);
    auto* area = addArea(*root);
    int cancellations = 0;
    int drops = 0;
    bool dragging = false;
    area->setOnMotion([&dragging](const InputArea::PointerData&) { dragging = true; });
    area->setOnPress([&](const InputArea::PointerData& data) {
      if (!data.pressed && dragging) {
        ++drops;
      }
    });
    area->setOnCancel([&]() {
      ++cancellations;
      dragging = false;
    });
    dispatcher.setSceneRoot(root.get());

    ok = expect(press(dispatcher), "pointer leave setup press is consumed") && ok;
    dispatcher.pointerMotion(20.0F, 20.0F, 2);
    ok = expect(dragging, "captured motion enters simulated dragging state") && ok;
    dispatcher.pointerLeave();

    ok = expect(cancellations == 1, "pointer leave cancels capture exactly once") && ok;
    ok = expect(!dispatcher.pointerCaptured(), "pointer leave clears capture") && ok;
    ok = expect(!area->pressed(), "pointer leave clears pressed state") && ok;
    ok = expect(!dragging, "pointer leave restores transient drag state") && ok;

    dispatcher.pointerEnter(10.0F, 10.0F, 3);
    dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, false, 0, 0, false);
    ok = expect(drops == 0, "release after pointer-leave cancellation does not drop") && ok;
    dispatcher.setSceneRoot(nullptr);
    ok = expect(cancellations == 1, "later scene reset does not duplicate cancellation") && ok;
  }

  {
    InputDispatcher dispatcher;
    auto root = std::make_unique<Node>();
    auto replacementRoot = std::make_unique<Node>();
    root->setSize(100.0F, 100.0F);
    replacementRoot->setSize(100.0F, 100.0F);
    auto* area = addArea(*root);
    int cancellations = 0;
    area->setOnCancel([&cancellations]() { ++cancellations; });
    dispatcher.setSceneRoot(root.get());

    ok = expect(press(dispatcher), "scene replacement setup press is consumed") && ok;
    dispatcher.setSceneRoot(replacementRoot.get());

    ok = expect(cancellations == 1, "scene replacement cancels capture exactly once") && ok;
    ok = expect(!dispatcher.pointerCaptured(), "scene replacement clears capture") && ok;
    ok = expect(!area->pressed(), "scene replacement clears pressed state") && ok;
  }

  {
    InputDispatcher dispatcher;
    auto root = std::make_unique<Node>();
    root->setSize(100.0F, 100.0F);
    auto* area = addArea(*root);
    int cancellations = 0;
    int sequence = 0;
    int cancellationOrder = 0;
    int leaveOrder = 0;
    area->setOnCancel([&]() {
      ++cancellations;
      cancellationOrder = ++sequence;
    });
    area->setOnLeave([&]() { leaveOrder = ++sequence; });
    dispatcher.setSceneRoot(root.get());

    ok = expect(press(dispatcher), "detachment setup press is consumed") && ok;
    auto detached = root->removeChild(area);
    dispatcher.pointerMotion(10.0F, 10.0F, 2);

    ok = expect(cancellations == 1, "detached captured area is cancelled exactly once") && ok;
    ok = expect(cancellationOrder == 1 && leaveOrder == 2, "detachment cancels capture before hover leave") && ok;
    ok = expect(!dispatcher.pointerCaptured(), "detachment clears capture") && ok;
    ok = expect(!area->pressed(), "detachment clears pressed state") && ok;
  }

  {
    InputDispatcher dispatcher;
    auto root = std::make_unique<Node>();
    root->setSize(100.0F, 100.0F);
    auto* area = addArea(*root);
    int cancellations = 0;
    std::uint32_t cursor = 0;
    area->setOnCancel([&cancellations]() { ++cancellations; });
    area->setCursorShape(19);
    dispatcher.setCursorShapeCallback([&cursor](std::uint32_t, std::uint32_t shape) { cursor = shape; });
    dispatcher.setSceneRoot(root.get());

    ok = expect(press(dispatcher), "destruction setup press is consumed") && ok;
    dispatcher.pointerMotion(10.0F, 10.0F, 2);
    ok = expect(cursor == 19, "captured area owns cursor before destruction") && ok;
    auto destroyed = root->removeChild(area);
    destroyed.reset();

    ok = expect(cancellations == 0, "destruction observer does not invoke cancellation callback") && ok;
    ok = expect(!dispatcher.pointerCaptured(), "destruction observer clears captured raw pointer") && ok;
    ok = expect(cursor == 1, "destruction observer restores the default cursor without a dangling access") && ok;
  }

  {
    constexpr std::uint32_t hoveredCursor = 17;
    constexpr std::uint32_t capturedCursor = 18;

    InputDispatcher dispatcher;
    auto root = std::make_unique<Node>();
    root->setSize(100.0F, 100.0F);

    auto capturedArea = std::make_unique<InputArea>();
    capturedArea->setSize(40.0F, 40.0F);
    capturedArea->setCursorShape(capturedCursor);

    auto hoveredArea = std::make_unique<InputArea>();
    hoveredArea->setSize(40.0F, 40.0F);
    hoveredArea->setCursorShape(hoveredCursor);
    hoveredArea->setAcceptedButtons(0);
    auto* hovered = static_cast<InputArea*>(capturedArea->addChild(std::move(hoveredArea)));
    auto* captured = static_cast<InputArea*>(root->addChild(std::move(capturedArea)));

    std::uint32_t cursor = 0;
    dispatcher.setCursorShapeCallback([&cursor](std::uint32_t, std::uint32_t shape) { cursor = shape; });
    dispatcher.setSceneRoot(root.get());
    dispatcher.pointerEnter(10.0F, 10.0F, 1);

    ok = expect(dispatcher.hoveredArea() == hovered, "deepest area is hovered before capture") && ok;
    ok = expect(cursor == hoveredCursor, "hovered area owns cursor before capture") && ok;

    ok = expect(
             dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, true, 0, 0, false),
             "cursor-authority setup press is consumed by accepting ancestor"
         )
        && ok;
    ok = expect(dispatcher.pointerCaptured(), "accepting ancestor captures pointer") && ok;
    dispatcher.pointerMotion(10.0F, 10.0F, 2);

    ok = expect(dispatcher.hoveredArea() == hovered, "hover remains on deepest area during capture") && ok;
    ok = expect(captured->pressed(), "accepting ancestor is the captured area") && ok;
    ok = expect(cursor == capturedCursor, "captured area owns cursor while capture is held") && ok;

    dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, false, 0, 0, false);
    ok = expect(cursor == hoveredCursor, "hovered area regains cursor after release") && ok;

    ok = expect(
             dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, true, 0, 0, false),
             "cursor-authority cancellation setup press is consumed"
         )
        && ok;
    dispatcher.pointerMotion(10.0F, 10.0F, 3);
    ok = expect(cursor == capturedCursor, "captured area owns cursor before cancellation") && ok;

    dispatcher.cancelPointerCapture();
    ok = expect(cursor == hoveredCursor, "hovered area regains cursor after cancellation") && ok;
  }

  {
    InputDispatcher dispatcher;
    TouchScrollScene scene;
    int rowPresses = 0;
    int rowClicks = 0;
    int viewportPresses = 0;
    scene.row->setOnPress([&rowPresses](const InputArea::PointerData& data) {
      if (data.pressed) {
        ++rowPresses;
      }
    });
    scene.row->setOnClick([&rowClicks](const InputArea::PointerData&) { ++rowClicks; });
    scene.viewport->setOnPress([&viewportPresses](const InputArea::PointerData&) { ++viewportPresses; });
    dispatcher.setSceneRoot(scene.root.get());
    dispatcher.pointerEnter(10.0F, 10.0F, 1);

    ok = expect(
             dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, true, 1, 10, true),
             "touch tap press is consumed while direction is pending"
         )
        && ok;
    ok = expect(rowPresses == 0, "touch tap press is deferred until release") && ok;
    ok = expect(viewportPresses == 0, "touch tap does not press the scroll viewport") && ok;

    (void)dispatcher.pointerButton(12.0F, 11.0F, BTN_LEFT, false, 2, 20, true);
    ok = expect(rowPresses == 1, "touch tap delivers the deferred child press") && ok;
    ok = expect(rowClicks == 1, "touch tap clicks the child") && ok;
    ok = expect(viewportPresses == 0, "touch tap remains unclaimed by the scroll viewport") && ok;
  }

  {
    InputDispatcher dispatcher;
    TouchScrollScene scene;
    int rowPresses = 0;
    int rowClicks = 0;
    int viewportPresses = 0;
    int viewportReleases = 0;
    int viewportMotions = 0;
    scene.row->setOnPress([&rowPresses](const InputArea::PointerData& data) {
      if (data.pressed) {
        ++rowPresses;
      }
    });
    scene.row->setOnClick([&rowClicks](const InputArea::PointerData&) { ++rowClicks; });
    scene.viewport->setOnPress([&](const InputArea::PointerData& data) {
      if (data.pressed) {
        ++viewportPresses;
      } else {
        ++viewportReleases;
      }
    });
    scene.viewport->setOnMotion([&viewportMotions](const InputArea::PointerData&) { ++viewportMotions; });
    dispatcher.setSceneRoot(scene.root.get());
    dispatcher.pointerEnter(10.0F, 10.0F, 1);

    (void)dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, true, 1, 10, true);
    dispatcher.pointerMotion(10.0F, 30.0F, 2);
    ok = expect(viewportPresses == 1, "vertical touch drag presses the scroll viewport at claim") && ok;
    ok = expect(viewportMotions == 1, "vertical touch drag forwards current motion to the viewport") && ok;
    ok = expect(rowPresses == 0, "vertical touch drag never presses the child") && ok;
    ok = expect(dispatcher.pointerCaptured(), "vertical touch drag captures the viewport") && ok;

    (void)dispatcher.pointerButton(10.0F, 30.0F, BTN_LEFT, false, 3, 20, true);
    ok = expect(viewportReleases == 1, "vertical touch drag releases the scroll viewport") && ok;
    ok = expect(rowClicks == 0, "vertical touch drag does not click the child") && ok;
  }

  {
    InputDispatcher dispatcher;
    TouchScrollScene scene;
    int rowPresses = 0;
    int rowMotions = 0;
    int viewportPresses = 0;
    scene.row->setOnPress([&rowPresses](const InputArea::PointerData& data) {
      if (data.pressed) {
        ++rowPresses;
      }
    });
    scene.row->setOnMotion([&rowMotions](const InputArea::PointerData&) { ++rowMotions; });
    scene.viewport->setOnPress([&viewportPresses](const InputArea::PointerData&) { ++viewportPresses; });
    dispatcher.setSceneRoot(scene.root.get());
    dispatcher.pointerEnter(10.0F, 10.0F, 1);

    (void)dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, true, 1, 10, true);
    dispatcher.pointerMotion(30.0F, 12.0F, 2);
    ok = expect(rowPresses == 1, "horizontal touch drag delivers the deferred child press") && ok;
    ok = expect(rowMotions == 1, "horizontal touch drag forwards current motion to the child") && ok;
    ok = expect(viewportPresses == 0, "horizontal touch drag is not claimed by a vertical viewport") && ok;
    (void)dispatcher.pointerButton(30.0F, 12.0F, BTN_LEFT, false, 3, 20, true);
  }

  {
    InputDispatcher dispatcher;
    TouchScrollScene scene;
    int rowPresses = 0;
    int rowClicks = 0;
    int viewportPresses = 0;
    scene.row->setOnPress([&rowPresses](const InputArea::PointerData&) { ++rowPresses; });
    scene.row->setOnClick([&rowClicks](const InputArea::PointerData&) { ++rowClicks; });
    scene.viewport->setOnPress([&viewportPresses](const InputArea::PointerData&) { ++viewportPresses; });
    dispatcher.setSceneRoot(scene.root.get());
    dispatcher.pointerEnter(10.0F, 10.0F, 1);

    (void)dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, true, 1, 10, true);
    dispatcher.pointerLeave();
    ok = expect(rowPresses == 0 && rowClicks == 0, "pointer leave drops a deferred touch press without a click") && ok;
    ok = expect(viewportPresses == 0, "pointer leave does not claim a deferred touch press") && ok;
    ok = expect(!dispatcher.pointerCaptured(), "pointer leave leaves no touch capture") && ok;
    ok = expect(
             !dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, false, 2, 20, true),
             "release after a deferred touch leave is ignored"
         )
        && ok;
  }

  {
    InputDispatcher dispatcher;
    TouchScrollScene scene;
    int rowPresses = 0;
    int viewportPresses = 0;
    scene.row->setOnPress([&rowPresses](const InputArea::PointerData& data) {
      if (data.pressed) {
        ++rowPresses;
      }
    });
    scene.viewport->setOnPress([&viewportPresses](const InputArea::PointerData&) { ++viewportPresses; });
    dispatcher.setSceneRoot(scene.root.get());
    dispatcher.pointerEnter(10.0F, 10.0F, 1);

    ok = expect(
             dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, true, 1, 10, false),
             "mouse press remains immediately consumed"
         )
        && ok;
    ok = expect(rowPresses == 1, "mouse press reaches the child immediately") && ok;
    ok = expect(viewportPresses == 0, "mouse press is not handed to the touch-scroll viewport") && ok;
    (void)dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, false, 2, 20, false);
  }

  return ok ? 0 : 1;
}
