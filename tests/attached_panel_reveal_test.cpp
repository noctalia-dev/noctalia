#include "shell/panel/attached_panel_context.h"
#include "tests/test_check.h"

#include <cmath>
#include <string_view>

int main() {
  constexpr AttachedPanelGeometry body{100, 40, 320, 480, 16};
  for (std::string_view edge : {"top", "bottom", "left", "right"}) {
    const bool vertical = edge == "left" || edge == "right";
    const float depth = vertical ? body.width : body.height;
    float previous = 0;
    for (int step = 0; step <= 100; ++step) {
      const float progress = static_cast<float>(step) / 100;
      const auto visible = attached_panel::revealGeometry(body, edge, progress);
      TEST_CHECK((vertical ? visible.width : visible.height) == std::round(depth * progress));
      TEST_CHECK(visible.bulgeRadius >= previous);
      previous = visible.bulgeRadius;
      if (vertical) {
        TEST_CHECK(std::abs((visible.y + visible.bulgeRadius) - body.y) < 0.001F);
        TEST_CHECK(std::abs((visible.height - 2 * visible.bulgeRadius) - body.height) < 0.001F);
        TEST_CHECK(edge == "left" ? visible.x == body.x : visible.x + visible.width == body.x + body.width);
      } else {
        TEST_CHECK(std::abs((visible.x + visible.bulgeRadius) - body.x) < 0.001F);
        TEST_CHECK(std::abs((visible.width - 2 * visible.bulgeRadius) - body.width) < 0.001F);
        TEST_CHECK(edge == "top" ? visible.y == body.y : visible.y + visible.height == body.y + body.height);
      }
      if (step == 0 || step == 100) {
        TEST_CHECK(visible.bulgeRadius == (step == 0 ? 0 : body.cornerRadius));
      } else if (step == 50) {
        TEST_CHECK(visible.bulgeRadius > 0 && visible.bulgeRadius < body.cornerRadius);
      }
    }
  }
  return 0;
}
