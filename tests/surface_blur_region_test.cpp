#include "shell/bar/bar_blur_policy.h"
#include "test_check.h"
#include "wayland/surface.h"

int main() {
  TEST_CHECK(!Surface::regionIntersectsBounds({InputRect{8, -39, 3056, 35}}, 3072, 49));

  TEST_CHECK(Surface::regionIntersectsBounds({InputRect{8, -34, 3056, 35}}, 3072, 49));

  BarConfig bar;
  TEST_CHECK(noctalia::bar::shouldPrewarmCompositorBlur(bar));
  TEST_CHECK(noctalia::bar::shouldPublishCompositorBlur(bar, true));
  TEST_CHECK(!noctalia::bar::shouldPublishCompositorBlur(bar, false));

  bar.compositorBlur = false;
  TEST_CHECK(!noctalia::bar::shouldPrewarmCompositorBlur(bar));
  TEST_CHECK(!noctalia::bar::shouldPublishCompositorBlur(bar, true));

  bar.compositorBlur = true;
  TEST_CHECK(noctalia::bar::shouldPrewarmCompositorBlur(bar));
  TEST_CHECK(noctalia::bar::shouldPublishCompositorBlur(bar, true));

  return 0;
}
