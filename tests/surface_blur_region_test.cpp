#include "shell/wayland/layer_surface.h"
#include "tests/test_check.h"
int main() {
  TEST_CHECK(!LayerSurface::regionIntersectsBounds({InputRect{8, -39, 3056, 35}}, 3072, 49));

  TEST_CHECK(LayerSurface::regionIntersectsBounds({InputRect{8, -34, 3056, 35}}, 3072, 49));

  return 0;
}
