#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"
#include "tests/test_check.h"

namespace {

  void resetMotion() {
    auto& motion = MotionService::instance();
    motion.setSpeed(1.0F);
    motion.setEnabled(true);
  }

} // namespace

int main() {
  resetMotion();

  {
    AnimationManager manager;
    MotionService::instance().setEnabled(false);

    float value = 0.0F;
    bool completed = false;
    const auto id = manager.animate(
        0.0F, 10.0F, 200.0F, Easing::Linear, [&value](float v) { value = v; }, [&completed] { completed = true; }
    );

    TEST_CHECK(id != 0);
    TEST_NEAR(value, 10.0F, 0.0001F);
    TEST_CHECK(!completed);

    manager.tick(0.0F);

    TEST_CHECK(completed);
    TEST_CHECK(!manager.hasActive());
  }

  resetMotion();

  {
    AnimationManager manager;

    float value = 0.0F;
    bool completed = false;
    const auto id = manager.animate(
        0.0F, 10.0F, 1000.0F, Easing::Linear, [&value](float v) { value = v; }, [&completed] { completed = true; }
    );

    TEST_CHECK(id != 0);

    MotionService::instance().setEnabled(false);

    TEST_NEAR(value, 10.0F, 0.0001F);
    TEST_CHECK(!completed);
    TEST_CHECK(manager.hasActive());

    manager.tick(0.0F);

    TEST_CHECK(completed);
  }

  resetMotion();

  {
    AnimationManager manager;
    MotionService::instance().setEnabled(false);

    float value = -1.0F;
    bool completed = false;
    const auto id = manager.animateTimer(
        1.0F, 0.0F, 1000.0F, Easing::Linear, [&value](float v) { value = v; }, [&completed] { completed = true; }
    );

    TEST_CHECK(id != 0);
    TEST_NEAR(value, -1.0F, 0.0001F);

    manager.tick(0.0F);

    TEST_CHECK(!completed);
    TEST_CHECK(manager.hasActive());
    manager.cancel(id);
  }

  resetMotion();
  return 0;
}
