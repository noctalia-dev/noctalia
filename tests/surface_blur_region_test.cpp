#include "test_check.h"
#include "wayland/surface.h"

int main() {
  TEST_CHECK(!Surface::regionIntersectsBounds({InputRect{8, -39, 3056, 35}}, 3072, 49));

  TEST_CHECK(Surface::regionIntersectsBounds({InputRect{8, -34, 3056, 35}}, 3072, 49));

  return 0;
}
