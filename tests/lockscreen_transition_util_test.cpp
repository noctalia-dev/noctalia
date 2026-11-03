#include "config/config_types.h"
#include "shell/lockscreen/lockscreen_transition_util.h"
#include "tests/test_check.h"

#include <cmath>
#include <set>

int main() {
  std::set<LockscreenTransitionKind> kinds;
  for (const auto& option : kLockscreenTransitions) {
    kinds.insert(lockscreen::renderTransitionKind(option.value));

    for (int i = 0; i < 200; ++i) {
      const LockscreenTransitionParams params = lockscreen::randomTransitionParams(option.value, 0.7F);
      TEST_CHECK(params.smoothness == 0.7F);
      TEST_CHECK(params.centerX >= 0.2F && params.centerX <= 0.8F);
      TEST_CHECK(params.centerY >= 0.2F && params.centerY <= 0.8F);
      TEST_CHECK(
          params.direction >= 0.0F && params.direction <= 3.0F && params.direction == std::floor(params.direction)
      );
      TEST_CHECK(params.stripeCount >= 4.0F && params.stripeCount <= 24.0F);
      TEST_CHECK(params.angle >= 0.0F && params.angle <= 360.0F);
      TEST_CHECK(params.cellSize >= 0.02F && params.cellSize <= 0.069F);
    }
  }
  TEST_CHECK(kinds.size() == std::size(kLockscreenTransitions));
  return 0;
}
