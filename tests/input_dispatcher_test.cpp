#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "tests/test_check.h"

#include <linux/input-event-codes.h>
#include <memory>

namespace {

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
  {
    Node root;
    root.setSize(100.0F, 100.0F);
    auto* area = addArea(root);
    area->setFocusable(true);
    InputDispatcher first, second;
    first.setSceneRoot(&root);
    second.setSceneRoot(&root);
    first.setFocus(area);
    second.setFocus(area);
    // First shared dispatcher captures the area.
    TEST_CHECK(press(first));
    // Second shared dispatcher captures the area.
    TEST_CHECK(press(second));

    root.removeChild(area).reset();
    // Shared area destruction clears focus in every dispatcher.
    TEST_CHECK(first.focusedArea() == nullptr && second.focusedArea() == nullptr);
    // Shared area destruction clears hover in every dispatcher.
    TEST_CHECK(first.hoveredArea() == nullptr && second.hoveredArea() == nullptr);
    // Shared area destruction clears capture in every dispatcher.
    TEST_CHECK(!first.pointerCaptured() && !second.pointerCaptured());
  }

  {
    Node left, right;
    left.setSize(100.0F, 100.0F);
    right.setSize(100.0F, 100.0F);
    auto* area = addArea(left);
    area->setFocusable(true);
    auto previous = std::make_unique<InputDispatcher>();
    previous->setSceneRoot(&left);
    previous->setFocus(area);
    previous->pointerEnter(10.0F, 10.0F, 1);
    right.addChild(left.removeChild(area));
    InputDispatcher current;
    current.setSceneRoot(&right);
    current.setFocus(area);
    // Reparented area is captured by its current dispatcher.
    TEST_CHECK(press(current));

    previous.reset();
    right.removeChild(area).reset();
    // Destroying the old dispatcher preserves the current destruction observer.
    TEST_CHECK(current.focusedArea() == nullptr && current.hoveredArea() == nullptr && !current.pointerCaptured());
  }

  {
    Node root;
    root.setSize(100.0F, 100.0F);
    auto* area = addArea(root);
    area->setFocusable(true);
    int staleNotifications = 0;
    auto dispatcher = std::make_unique<InputDispatcher>();
    dispatcher->setSceneRoot(&root);
    dispatcher->setFocus(area);
    dispatcher->setHoverChangeCallback([&](InputArea* oldArea, InputArea* newArea) {
      if (oldArea != nullptr && newArea == nullptr) {
        ++staleNotifications;
      }
    });
    // Expired dispatcher captures the surviving area.
    TEST_CHECK(press(*dispatcher));

    dispatcher.reset();
    root.removeChild(area).reset();
    // Area destruction never calls a destroyed dispatcher.
    TEST_CHECK(staleNotifications == 0);
  }

  {
    InputDispatcher dispatcher;
    auto root = std::make_unique<Node>();
    root->setSize(100.0F, 100.0F);
    auto* area = addArea(*root);
    InputArea::PointerData clicked;
    area->setOnClick([&clicked](const InputArea::PointerData& data) { clicked = data; });
    dispatcher.setSceneRoot(root.get());
    dispatcher.pointerEnter(10.0F, 12.0F, 40);
    dispatcher.pointerButton(10.0F, 12.0F, BTN_LEFT, true, 41, 100, false);
    dispatcher.pointerButton(11.0F, 13.0F, BTN_LEFT, false, 42, 101, false);
    // Click callbacks preserve the initiating press coordinates, serial, and time.
    TEST_CHECK(clicked.sceneX == 10.0F && clicked.sceneY == 12.0F && clicked.serial == 41 && clicked.time == 100);
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
    area->setOnCancel([&] {
      ++cancellations;
      captureVisibleDuringCallback = dispatcher.pointerCaptured();
      pressedClearedBeforeCallback = !area->pressed();
      dispatcher.cancelPointerCapture();
    });
    area->setOnClick([&clicks](const InputArea::PointerData&) { ++clicks; });
    dispatcher.setSceneRoot(root.get());

    // Explicit cancellation setup press is consumed.
    TEST_CHECK(press(dispatcher));
    dispatcher.cancelPointerCapture();
    dispatcher.cancelPointerCapture();

    // Explicit cancellation fires callback once.
    TEST_CHECK(cancellations == 1);
    // Capture remains visible during cancellation callback.
    TEST_CHECK(captureVisibleDuringCallback);
    // Pressed state clears before cancellation callback.
    TEST_CHECK(pressedClearedBeforeCallback);
    // Explicit cancellation clears capture.
    TEST_CHECK(!dispatcher.pointerCaptured());
    // Explicit cancellation clears pressed state.
    TEST_CHECK(!area->pressed());

    dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, false, 0, 0, false);
    // Release after explicit cancellation does not click.
    TEST_CHECK(clicks == 0);
  }

  {
    InputDispatcher dispatcher;
    auto root = std::make_unique<Node>();
    root->setSize(100.0F, 100.0F);
    auto* area = addArea(*root);
    int cancellations = 0;
    area->setOnCancel([&cancellations] { ++cancellations; });
    dispatcher.setSceneRoot(root.get());

    // Armed pointer-leave setup press is consumed.
    TEST_CHECK(press(dispatcher));
    dispatcher.pointerLeave();

    // Pointer leave cancels armed capture exactly once.
    TEST_CHECK(cancellations == 1);
    // Armed pointer leave clears capture.
    TEST_CHECK(!dispatcher.pointerCaptured());
    // Armed pointer leave clears pressed state.
    TEST_CHECK(!area->pressed());
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
    area->setOnCancel([&] {
      ++cancellations;
      dragging = false;
    });
    dispatcher.setSceneRoot(root.get());

    // Pointer leave setup press is consumed.
    TEST_CHECK(press(dispatcher));
    dispatcher.pointerMotion(20.0F, 20.0F, 2);
    // Captured motion enters simulated dragging state.
    TEST_CHECK(dragging);
    dispatcher.pointerLeave();

    // Pointer leave cancels capture exactly once.
    TEST_CHECK(cancellations == 1);
    // Pointer leave clears capture.
    TEST_CHECK(!dispatcher.pointerCaptured());
    // Pointer leave clears pressed state.
    TEST_CHECK(!area->pressed());
    // Pointer leave restores transient drag state.
    TEST_CHECK(!dragging);

    dispatcher.pointerEnter(10.0F, 10.0F, 3);
    dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, false, 0, 0, false);
    // Release after pointer-leave cancellation does not drop.
    TEST_CHECK(drops == 0);
    dispatcher.setSceneRoot(nullptr);
    // Later scene reset does not duplicate cancellation.
    TEST_CHECK(cancellations == 1);
  }

  {
    InputDispatcher dispatcher;
    auto root = std::make_unique<Node>();
    auto replacementRoot = std::make_unique<Node>();
    root->setSize(100.0F, 100.0F);
    replacementRoot->setSize(100.0F, 100.0F);
    auto* area = addArea(*root);
    int cancellations = 0;
    area->setOnCancel([&cancellations] { ++cancellations; });
    dispatcher.setSceneRoot(root.get());

    // Scene replacement setup press is consumed.
    TEST_CHECK(press(dispatcher));
    dispatcher.setSceneRoot(replacementRoot.get());

    // Scene replacement cancels capture exactly once.
    TEST_CHECK(cancellations == 1);
    // Scene replacement clears capture.
    TEST_CHECK(!dispatcher.pointerCaptured());
    // Scene replacement clears pressed state.
    TEST_CHECK(!area->pressed());
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
    area->setOnCancel([&] {
      ++cancellations;
      cancellationOrder = ++sequence;
    });
    area->setOnLeave([&] { leaveOrder = ++sequence; });
    dispatcher.setSceneRoot(root.get());

    // Detachment setup press is consumed.
    TEST_CHECK(press(dispatcher));
    auto detached = root->removeChild(area);
    dispatcher.pointerMotion(10.0F, 10.0F, 2);

    // Detached captured area is cancelled exactly once.
    TEST_CHECK(cancellations == 1);
    // Detachment cancels capture before hover leave.
    TEST_CHECK(cancellationOrder == 1 && leaveOrder == 2);
    // Detachment clears capture.
    TEST_CHECK(!dispatcher.pointerCaptured());
    // Detachment clears pressed state.
    TEST_CHECK(!area->pressed());
  }

  {
    InputDispatcher dispatcher;
    auto root = std::make_unique<Node>();
    root->setSize(100.0F, 100.0F);
    auto* area = addArea(*root);
    int cancellations = 0;
    std::uint32_t cursor = 0;
    area->setOnCancel([&cancellations] { ++cancellations; });
    area->setCursorShape(19);
    dispatcher.setCursorShapeCallback([&cursor](std::uint32_t, std::uint32_t shape) { cursor = shape; });
    dispatcher.setSceneRoot(root.get());

    // Destruction setup press is consumed.
    TEST_CHECK(press(dispatcher));
    dispatcher.pointerMotion(10.0F, 10.0F, 2);
    // Captured area owns cursor before destruction.
    TEST_CHECK(cursor == 19);
    auto destroyed = root->removeChild(area);
    destroyed.reset();

    // Destruction observer does not invoke cancellation callback.
    TEST_CHECK(cancellations == 0);
    // Destruction observer clears captured raw pointer.
    TEST_CHECK(!dispatcher.pointerCaptured());
    // Destruction observer restores the default cursor without a dangling access.
    TEST_CHECK(cursor == 1);
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

    // Deepest area is hovered before capture.
    TEST_CHECK(dispatcher.hoveredArea() == hovered);
    // Hovered area owns cursor before capture.
    TEST_CHECK(cursor == hoveredCursor);

    // Cursor-authority setup press is consumed by accepting ancestor.
    TEST_CHECK(dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, true, 0, 0, false));
    // Accepting ancestor captures pointer.
    TEST_CHECK(dispatcher.pointerCaptured());
    dispatcher.pointerMotion(10.0F, 10.0F, 2);

    // Hover remains on deepest area during capture.
    TEST_CHECK(dispatcher.hoveredArea() == hovered);
    // Accepting ancestor is the captured area.
    TEST_CHECK(captured->pressed());
    // Captured area owns cursor while capture is held.
    TEST_CHECK(cursor == capturedCursor);

    dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, false, 0, 0, false);
    // Hovered area regains cursor after release.
    TEST_CHECK(cursor == hoveredCursor);

    // Cursor-authority cancellation setup press is consumed.
    TEST_CHECK(dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, true, 0, 0, false));
    dispatcher.pointerMotion(10.0F, 10.0F, 3);
    // Captured area owns cursor before cancellation.
    TEST_CHECK(cursor == capturedCursor);

    dispatcher.cancelPointerCapture();
    // Hovered area regains cursor after cancellation.
    TEST_CHECK(cursor == hoveredCursor);
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

    // Touch tap press is consumed while direction is pending.
    TEST_CHECK(dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, true, 1, 10, true));
    // Touch tap press is deferred until release.
    TEST_CHECK(rowPresses == 0);
    // Touch tap does not press the scroll viewport.
    TEST_CHECK(viewportPresses == 0);

    dispatcher.pointerButton(12.0F, 11.0F, BTN_LEFT, false, 2, 20, true);
    // Touch tap delivers the deferred child press.
    TEST_CHECK(rowPresses == 1);
    // Touch tap clicks the child.
    TEST_CHECK(rowClicks == 1);
    // Touch tap remains unclaimed by the scroll viewport.
    TEST_CHECK(viewportPresses == 0);
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

    dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, true, 1, 10, true);
    dispatcher.pointerMotion(10.0F, 30.0F, 2);
    // Vertical touch drag presses the scroll viewport at claim.
    TEST_CHECK(viewportPresses == 1);
    // Vertical touch drag forwards current motion to the viewport.
    TEST_CHECK(viewportMotions == 1);
    // Vertical touch drag never presses the child.
    TEST_CHECK(rowPresses == 0);
    // Vertical touch drag captures the viewport.
    TEST_CHECK(dispatcher.pointerCaptured());

    dispatcher.pointerButton(10.0F, 30.0F, BTN_LEFT, false, 3, 20, true);
    // Vertical touch drag releases the scroll viewport.
    TEST_CHECK(viewportReleases == 1);
    // Vertical touch drag does not click the child.
    TEST_CHECK(rowClicks == 0);
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

    dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, true, 1, 10, true);
    dispatcher.pointerMotion(30.0F, 12.0F, 2);
    // Horizontal touch drag delivers the deferred child press.
    TEST_CHECK(rowPresses == 1);
    // Horizontal touch drag forwards current motion to the child.
    TEST_CHECK(rowMotions == 1);
    // Horizontal touch drag is not claimed by a vertical viewport.
    TEST_CHECK(viewportPresses == 0);
    dispatcher.pointerButton(30.0F, 12.0F, BTN_LEFT, false, 3, 20, true);
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

    dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, true, 1, 10, true);
    dispatcher.pointerLeave();
    // Pointer leave drops a deferred touch press without a click.
    TEST_CHECK(rowPresses == 0 && rowClicks == 0);
    // Pointer leave does not claim a deferred touch press.
    TEST_CHECK(viewportPresses == 0);
    // Pointer leave leaves no touch capture.
    TEST_CHECK(!dispatcher.pointerCaptured());
    // Release after a deferred touch leave is ignored.
    TEST_CHECK(!dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, false, 2, 20, true));
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

    // Mouse press remains immediately consumed.
    TEST_CHECK(dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, true, 1, 10, false));
    // Mouse press reaches the child immediately.
    TEST_CHECK(rowPresses == 1);
    // Mouse press is not handed to the touch-scroll viewport.
    TEST_CHECK(viewportPresses == 0);
    dispatcher.pointerButton(10.0F, 10.0F, BTN_LEFT, false, 2, 20, false);
  }

  return 0;
}
