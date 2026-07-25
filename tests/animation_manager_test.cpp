#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"

#include <cmath>
#include <iostream>

namespace {

  bool check(bool cond, const char* msg) {
    if (!cond) {
      std::cerr << "FAIL: " << msg << '\n';
    }
    return cond;
  }

  bool nearlyEqual(float a, float b) { return std::fabs(a - b) < 0.0001f; }

  void resetMotion() {
    auto& motion = MotionService::instance();
    motion.setSpeed(1.0f);
    motion.setEnabled(true);
  }

} // namespace

int main() {
  bool ok = true;
  resetMotion();

  {
    AnimationManager manager;
    MotionService::instance().setEnabled(false);

    float value = 0.0f;
    bool completed = false;
    const auto id = manager.animate(
        0.0f, 10.0f, 200.0f, Easing::Linear, [&value](float v) { value = v; }, [&completed]() { completed = true; }
    );

    ok &= check(id != 0, "reduced-motion animation with completion should remain cancellable");
    ok &= check(nearlyEqual(value, 10.0f), "reduced-motion animation did not snap to target");
    ok &= check(!completed, "reduced-motion completion ran synchronously");

    manager.tick(0.0f);

    ok &= check(completed, "reduced-motion completion did not run on tick");
    ok &= check(!manager.hasActive(), "reduced-motion animation stayed active after completion");
  }

  resetMotion();

  {
    AnimationManager manager;

    float value = 0.0f;
    bool completed = false;
    const auto id = manager.animate(
        0.0f, 10.0f, 1000.0f, Easing::Linear, [&value](float v) { value = v; }, [&completed]() { completed = true; }
    );

    ok &= check(id != 0, "normal animation did not start");

    MotionService::instance().setEnabled(false);

    ok &= check(nearlyEqual(value, 10.0f), "active animation did not snap when motion was disabled");
    ok &= check(!completed, "active animation completion ran synchronously when motion was disabled");
    ok &= check(manager.hasActive(), "active animation did not remain pending for async completion");

    manager.tick(0.0f);

    ok &= check(completed, "active animation completion did not run after reduced-motion snap");
  }

  resetMotion();

  {
    AnimationManager manager;
    MotionService::instance().setEnabled(false);

    float value = -1.0f;
    bool completed = false;
    const auto id = manager.animateTimer(
        1.0f, 0.0f, 1000.0f, Easing::Linear, [&value](float v) { value = v; }, [&completed]() { completed = true; }
    );

    ok &= check(id != 0, "timer animation did not start while motion was disabled");
    ok &= check(nearlyEqual(value, -1.0f), "timer animation snapped when motion was disabled");

    manager.tick(0.0f);

    ok &= check(!completed, "timer animation completed early while motion was disabled");
    ok &= check(manager.hasActive(), "timer animation was removed early while motion was disabled");
    manager.cancel(id);
  }

  resetMotion();
  return ok ? 0 : 1;
}
