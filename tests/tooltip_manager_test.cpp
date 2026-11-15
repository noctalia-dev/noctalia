#include "core/deferred_call.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "shell/tooltip/tooltip_manager.h"
#include "tests/test_check.h"
#include "wayland/popup_surface.h"
#include "xdg-shell-client-protocol.h"

#include <chrono>
#include <cmath>
#include <thread>

class TooltipManagerTestAccess {
public:
  static bool pending(TooltipManager& manager) {
    return manager.m_state == TooltipManager::State::Pending && manager.m_showTimer.active();
  }
  static bool idle(TooltipManager& manager) {
    return manager.m_state == TooltipManager::State::Idle && manager.m_pendingArea == nullptr
        && !manager.m_showTimer.active();
  }
  static void queueRetarget(TooltipManager& manager) { manager.scheduleRetargetPopup(); }
  static void showing(TooltipManager& manager) { manager.m_state = TooltipManager::State::Showing; }
  static void queueReshow(TooltipManager& manager) { manager.scheduleReshow(); }
  static void fading(TooltipManager& manager) { manager.m_state = TooltipManager::State::FadingOut; }
  static void queueDestroy(TooltipManager& manager) { manager.scheduleDestroyPopup(); }
  static bool queuedReshow(TooltipManager& manager) { return manager.m_reshowQueued; }
  static auto anchor(TooltipManager& manager, InputArea& area) { return manager.anchorConfig(&area); }
  static bool fresh(TooltipManager& manager) { return manager.m_freshHover; }
};

int main() {
  auto& manager = TooltipManager::instance();
  bool menu = false, drag = false, selection = false;
  manager.setFreshHoverPolicy([&] { return menu || drag || selection; });
  // Opaque parent handles are never dereferenced: these tests do not create a
  // Wayland connection or popup. Only the scheduling/lifetime paths execute.
  auto* parent = reinterpret_cast<xdg_surface*>(1);
  auto* output = reinterpret_cast<wl_output*>(1);
  InputArea first, second;
  first.setTooltip("First");
  second.setTooltip("Second");
  first.dispatchEnter(1, 1);
  second.dispatchEnter(1, 1);
  auto hover = [&](InputArea* area) { manager.onHoverChange(area, parent, output); };
  hover(&first);
  TEST_CHECK(TooltipManagerTestAccess::pending(manager));
  TEST_CHECK(TimerManager::instance().pollTimeoutMs() > 400);
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  TimerManager::instance().tick();
  TEST_CHECK(TooltipManagerTestAccess::pending(manager));
  TooltipManagerTestAccess::showing(manager);
  hover(&second);
  TEST_CHECK(TooltipManagerTestAccess::pending(manager));
  TEST_CHECK(TimerManager::instance().pollTimeoutMs() > 400);
  // Old deferred retargets must not bypass the new target's delay.
  TooltipManagerTestAccess::queueRetarget(manager);
  hover(&first);
  for (auto& callback : DeferredCall::takePending())
    callback();
  TEST_CHECK(TooltipManagerTestAccess::pending(manager));
  // Repeated host validation must not restart the delay for the same target.
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  hover(&first);
  TEST_CHECK(TimerManager::instance().pollTimeoutMs() < 400);
  // Recycling a still-hovered area restarts the delay; hiding it invalidates it.
  first.setTooltip("Recycled entry");
  TEST_CHECK(TimerManager::instance().pollTimeoutMs() > 400);
  first.setVisible(false);
  first.requestTooltipRefresh();
  TEST_CHECK(TooltipManagerTestAccess::idle(manager));
  first.setVisible(true);
  for (int i = 0; i < 20; ++i) {
    hover(i % 2 ? &first : &second);
    TEST_CHECK(TooltipManagerTestAccess::pending(manager));
  }
  for (bool* suppression : {&menu, &drag, &selection}) {
    *suppression = true;
    hover(&second);
    TEST_CHECK(TooltipManagerTestAccess::idle(manager));
    *suppression = false;
    hover(&second);
    TEST_CHECK(TooltipManagerTestAccess::pending(manager));
    TEST_CHECK(TimerManager::instance().pollTimeoutMs() > 400);
  }
  TooltipManagerTestAccess::showing(manager);
  hover(nullptr);
  TEST_CHECK(TooltipManagerTestAccess::idle(manager));
  std::this_thread::sleep_for(std::chrono::milliseconds(510));
  TimerManager::instance().tick();
  TEST_CHECK(TooltipManagerTestAccess::idle(manager));

  for (auto queue : {TooltipManagerTestAccess::queueRetarget, TooltipManagerTestAccess::queueReshow,
                     TooltipManagerTestAccess::queueDestroy}) {
    hover(&first);
    TooltipManagerTestAccess::fading(manager);
    queue(manager);
    hover(nullptr);
    hover(&second);
    for (auto& callback : DeferredCall::takePending())
      callback();
    TEST_CHECK(TooltipManagerTestAccess::pending(manager));
    TEST_CHECK(TimerManager::instance().pollTimeoutMs() > 400);
  }
  hover(nullptr);

  // Logical parent-surface coordinates are preserved, including edge positions;
  // the compositor resolves flip/slide constraints against the actual output.
  for (auto [x, y] : {std::pair{0.0F, 0.0F}, std::pair{1119.0F, 759.0F}, std::pair{21.5F, 42.5F}}) {
    manager.setPointerPosition(x, y);
    const auto config = TooltipManagerTestAccess::anchor(manager, first);
    TEST_CHECK(config.anchorX == std::lround(x) && config.anchorY == std::lround(y));
    TEST_CHECK(config.anchorWidth == 1 && config.anchorHeight == 1);
    TEST_CHECK(config.offsetX > 0 && config.offsetY > 0);
    TEST_CHECK(config.constraintAdjustment == (XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X
        | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
        | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y));
  }
  first.setPosition(10, 20);
  first.setSize(180, 30);
  first.setTooltipPlacement(TooltipPlacement::Right);
  const auto side = TooltipManagerTestAccess::anchor(manager, first);
  TEST_CHECK(side.anchorX == 190 && side.anchorY == 20 && side.anchorHeight == 30);
  TEST_CHECK(side.anchor == XDG_POSITIONER_ANCHOR_RIGHT && side.offsetX > 0);

  Node root;
  root.setSize(100, 100);
  InputDispatcher dispatcher;
  dispatcher.setSceneRoot(&root);
  dispatcher.setHoverChangeCallback([&](InputArea*, InputArea* next) { hover(next); });
  auto target = std::make_unique<InputArea>();
  target->setSize(100, 100);
  target->setTooltip("Removed target");
  auto* targetPtr = root.addChild(std::move(target));
  dispatcher.pointerEnter(5, 5, 0);
  TEST_CHECK(TooltipManagerTestAccess::pending(manager));
  TooltipManagerTestAccess::queueReshow(manager);
  auto removed = root.removeChild(targetPtr);
  removed.reset();
  for (auto& callback : DeferredCall::takePending())
    callback();
  TEST_CHECK(TooltipManagerTestAccess::idle(manager));
  // Layout can move a live area away without a pointer-motion event.
  auto moved = std::make_unique<InputArea>();
  moved->setSize(100, 100);
  moved->setTooltip("Moved");
  auto* movedPtr = root.addChild(std::move(moved));
  dispatcher.syncPointerHover();
  TEST_CHECK(TooltipManagerTestAccess::pending(manager));
  movedPtr->setPosition(200, 200);
  dispatcher.syncPointerHover();
  TEST_CHECK(TooltipManagerTestAccess::idle(manager));
  movedPtr->setPosition(0, 0);
  dispatcher.syncPointerHover();
  manager.setFreshHoverPolicy([&] { return dispatcher.pointerCaptured(); });
  hover(dispatcher.hoveredArea());
  dispatcher.pointerButton(5, 5, BTN_LEFT, true, 1, 0, false);
  TEST_CHECK(dispatcher.pointerCaptured());
  hover(dispatcher.hoveredArea());
  TEST_CHECK(TooltipManagerTestAccess::idle(manager));
  dispatcher.pointerButton(5, 5, BTN_LEFT, false, 1, 0, false);
  TEST_CHECK(!dispatcher.pointerCaptured());
  hover(dispatcher.hoveredArea());
  TEST_CHECK(TooltipManagerTestAccess::pending(manager));
  dispatcher.pointerLeave();
  TEST_CHECK(TooltipManagerTestAccess::idle(manager));
  manager.shutdown();
  // The shell still queues immediate retargeting while showing/fading.
  hover(&first);
  TooltipManagerTestAccess::showing(manager);
  hover(&second);
  TEST_CHECK(TooltipManagerTestAccess::queuedReshow(manager));
  TEST_CHECK(!TooltipManagerTestAccess::pending(manager));
  hover(nullptr);
  TooltipManagerTestAccess::fading(manager);
  hover(&first);
  TEST_CHECK(TooltipManagerTestAccess::queuedReshow(manager));
  const auto shellAnchor = TooltipManagerTestAccess::anchor(manager, first);
  TEST_CHECK(shellAnchor.constraintAdjustment == XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
  manager.shutdown();
  for (auto& callback : DeferredCall::takePending())
    callback();
  TEST_CHECK(!TooltipManagerTestAccess::fresh(manager));
}
