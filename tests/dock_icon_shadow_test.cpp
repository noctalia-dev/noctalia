#include "shell/dock/icon_shadow.h"
#include "tests/test_check.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {
  float alpha(const LoadedImageFile& image, int x, int y) {
    return image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4U + 3U] / 255.0F;
  }
} // namespace

int main() {
  using shell::dock::makeIconShadow;
  LoadedImageFile source{.rgba = std::vector<std::uint8_t>(21 * 21 * 4, 0), .width = 21, .height = 21};
  source.rgba[(10 * 21 + 10) * 4 + 3] = 255;
  const auto original = source.rgba;
  const auto mask = makeIconShadow(source, 21, 1);
  TEST_CHECK(source.rgba == original);
  TEST_CHECK(mask.width == 33 && mask.height == 33);
  TEST_CHECK(alpha(mask, 16, 16) > alpha(mask, 18, 16));
  float mass = 0;
  float variance = 0;
  for (int y = 0; y < mask.height; ++y) {
    for (int x = 0; x < mask.width; ++x) {
      TEST_CHECK(alpha(mask, x, y) == alpha(mask, 32 - x, y));
      TEST_CHECK(alpha(mask, x, y) == alpha(mask, x, 32 - y));
      mass += alpha(mask, x, y);
      variance += (x - 16) * (x - 16) * alpha(mask, x, y);
    }
  }
  TEST_CHECK(std::abs(mass - 1) < 0.1F);
  TEST_CHECK(std::abs(variance / mass - 4) < 0.8F);

  // A non-square opaque source must stay centered and preserve its aspect ratio.
  source = {.rgba = std::vector<std::uint8_t>(24 * 8 * 4, 255), .width = 24, .height = 8};
  for (int density : {1, 2, 3}) {
    const auto wide = makeIconShadow(source, 24, density);
    const int center = wide.width / 2;
    TEST_CHECK(alpha(wide, center, center) > 0.9F);
    TEST_CHECK(alpha(wide, center + 8 * density, center) > 0.9F);
    TEST_CHECK(alpha(wide, center, center + 8 * density) < 0.1F);
    TEST_CHECK(alpha(wide, 0, 0) == 0);
    float total = 0;
    for (int y = 0; y < wide.height; ++y) {
      for (int x = 0; x < wide.width; ++x) {
        TEST_CHECK(alpha(wide, x, y) == alpha(wide, wide.width - 1 - x, y));
        TEST_CHECK(alpha(wide, x, y) == alpha(wide, x, wide.height - 1 - y));
        total += alpha(wide, x, y);
      }
    }
    TEST_CHECK(std::abs(total / (density * density) - 24 * 8) < 2);
  }
  std::fill(source.rgba.begin(), source.rgba.end(), 0);
  const auto empty = makeIconShadow(source, 24, 1);
  for (std::size_t i = 3; i < empty.rgba.size(); i += 4) {
    TEST_CHECK(empty.rgba[i] == 0);
  }
  TEST_CHECK(makeIconShadow({}, 24, 1).rgba.empty());
}
