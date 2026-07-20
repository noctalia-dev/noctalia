#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"

#include <cstdio>
#include <linux/input-event-codes.h>
#include <memory>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::fprintf(stderr, "input_dispatcher_test: %s\n", message);
      return false;
    }
    return true;
  }

  InputArea* addArea(Node& root) {
    auto area = std::make_unique<InputArea>();
    area->setSize(40.0f, 40.0f);
    return static_cast<InputArea*>(root.addChild(std::move(area)));
  }

  bool press(InputDispatcher& dispatcher) {
    dispatcher.pointerEnter(10.0f, 10.0f, 1);
    return dispatcher.pointerButton(10.0f, 10.0f, BTN_LEFT, true);
  }

} // namespace

int main() {
  bool ok = true;

  {
    InputDispatcher dispatcher;
    auto root = std::make_unique<Node>();
    root->setSize(100.0f, 100.0f);
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

    dispatcher.pointerButton(10.0f, 10.0f, BTN_LEFT, false);
    ok = expect(clicks == 0, "release after explicit cancellation does not click") && ok;
  }

  {
    InputDispatcher dispatcher;
    auto root = std::make_unique<Node>();
    root->setSize(100.0f, 100.0f);
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
    root->setSize(100.0f, 100.0f);
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
    dispatcher.pointerMotion(20.0f, 20.0f, 2);
    ok = expect(dragging, "captured motion enters simulated dragging state") && ok;
    dispatcher.pointerLeave();

    ok = expect(cancellations == 1, "pointer leave cancels capture exactly once") && ok;
    ok = expect(!dispatcher.pointerCaptured(), "pointer leave clears capture") && ok;
    ok = expect(!area->pressed(), "pointer leave clears pressed state") && ok;
    ok = expect(!dragging, "pointer leave restores transient drag state") && ok;

    dispatcher.pointerEnter(10.0f, 10.0f, 3);
    dispatcher.pointerButton(10.0f, 10.0f, BTN_LEFT, false);
    ok = expect(drops == 0, "release after pointer-leave cancellation does not drop") && ok;
    dispatcher.setSceneRoot(nullptr);
    ok = expect(cancellations == 1, "later scene reset does not duplicate cancellation") && ok;
  }

  {
    InputDispatcher dispatcher;
    auto root = std::make_unique<Node>();
    auto replacementRoot = std::make_unique<Node>();
    root->setSize(100.0f, 100.0f);
    replacementRoot->setSize(100.0f, 100.0f);
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
    root->setSize(100.0f, 100.0f);
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
    dispatcher.pointerMotion(10.0f, 10.0f, 2);

    ok = expect(cancellations == 1, "detached captured area is cancelled exactly once") && ok;
    ok = expect(cancellationOrder == 1 && leaveOrder == 2, "detachment cancels capture before hover leave") && ok;
    ok = expect(!dispatcher.pointerCaptured(), "detachment clears capture") && ok;
    ok = expect(!area->pressed(), "detachment clears pressed state") && ok;
  }

  {
    InputDispatcher dispatcher;
    auto root = std::make_unique<Node>();
    root->setSize(100.0f, 100.0f);
    auto* area = addArea(*root);
    int cancellations = 0;
    std::uint32_t cursor = 0;
    area->setOnCancel([&cancellations]() { ++cancellations; });
    area->setCursorShape(19);
    dispatcher.setCursorShapeCallback([&cursor](std::uint32_t, std::uint32_t shape) { cursor = shape; });
    dispatcher.setSceneRoot(root.get());

    ok = expect(press(dispatcher), "destruction setup press is consumed") && ok;
    dispatcher.pointerMotion(10.0f, 10.0f, 2);
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
    root->setSize(100.0f, 100.0f);

    auto capturedArea = std::make_unique<InputArea>();
    capturedArea->setSize(40.0f, 40.0f);
    capturedArea->setCursorShape(capturedCursor);

    auto hoveredArea = std::make_unique<InputArea>();
    hoveredArea->setSize(40.0f, 40.0f);
    hoveredArea->setCursorShape(hoveredCursor);
    hoveredArea->setAcceptedButtons(0);
    auto* hovered = static_cast<InputArea*>(capturedArea->addChild(std::move(hoveredArea)));
    auto* captured = static_cast<InputArea*>(root->addChild(std::move(capturedArea)));

    std::uint32_t cursor = 0;
    dispatcher.setCursorShapeCallback([&cursor](std::uint32_t, std::uint32_t shape) { cursor = shape; });
    dispatcher.setSceneRoot(root.get());
    dispatcher.pointerEnter(10.0f, 10.0f, 1);

    ok = expect(dispatcher.hoveredArea() == hovered, "deepest area is hovered before capture") && ok;
    ok = expect(cursor == hoveredCursor, "hovered area owns cursor before capture") && ok;

    ok = expect(
             dispatcher.pointerButton(10.0f, 10.0f, BTN_LEFT, true),
             "cursor-authority setup press is consumed by accepting ancestor"
         )
        && ok;
    ok = expect(dispatcher.pointerCaptured(), "accepting ancestor captures pointer") && ok;
    dispatcher.pointerMotion(10.0f, 10.0f, 2);

    ok = expect(dispatcher.hoveredArea() == hovered, "hover remains on deepest area during capture") && ok;
    ok = expect(captured->pressed(), "accepting ancestor is the captured area") && ok;
    ok = expect(cursor == capturedCursor, "captured area owns cursor while capture is held") && ok;

    dispatcher.pointerButton(10.0f, 10.0f, BTN_LEFT, false);
    ok = expect(cursor == hoveredCursor, "hovered area regains cursor after release") && ok;

    ok = expect(
             dispatcher.pointerButton(10.0f, 10.0f, BTN_LEFT, true),
             "cursor-authority cancellation setup press is consumed"
         )
        && ok;
    dispatcher.pointerMotion(10.0f, 10.0f, 3);
    ok = expect(cursor == capturedCursor, "captured area owns cursor before cancellation") && ok;

    dispatcher.cancelPointerCapture();
    ok = expect(cursor == hoveredCursor, "hovered area regains cursor after cancellation") && ok;
  }

  return ok ? 0 : 1;
}
