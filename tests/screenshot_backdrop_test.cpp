#include "capture/screenshot_backdrop.h"
#include "test_check.h"

#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

  ScreencopyImage solidImage(int width, int height, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    ScreencopyImage image{
        .width = width,
        .height = height,
        .yInvert = false,
        .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4U),
    };
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
      image.rgba[i + 0] = r;
      image.rgba[i + 1] = g;
      image.rgba[i + 2] = b;
      image.rgba[i + 3] = 255;
    }
    return image;
  }

  struct Pixel {
    int r;
    int g;
    int b;
    int a;
  };

  Pixel pixelAt(const ScreencopyImage& image, int x, int y) {
    const std::size_t offset = ((static_cast<std::size_t>(y) * image.width) + x) * 4U;
    return Pixel{image.rgba[offset], image.rgba[offset + 1], image.rgba[offset + 2], image.rgba[offset + 3]};
  }

  bool near(int value, int expected, int tolerance = 2) { return std::abs(value - expected) <= tolerance; }

  capture::ScreenshotBackdrop plainBackdrop() {
    capture::ScreenshotBackdrop backdrop;
    backdrop.gradient = false;
    backdrop.shadow = false;
    backdrop.cornerRadius = 0;
    backdrop.padding = 0;
    backdrop.gradientStart = Color{1.0F, 0.0F, 0.0F, 1.0F};
    backdrop.gradientEnd = Color{0.0F, 0.0F, 1.0F, 1.0F};
    backdrop.shadowColor = Color{0.0F, 0.0F, 0.0F, 1.0F};
    return backdrop;
  }

  void testPassThrough() {
    const auto source = solidImage(8, 6, 10, 20, 30);
    const auto out = capture::applyScreenshotBackdrop(source, plainBackdrop());
    TEST_CHECK(out.width == 8 && out.height == 6);
    TEST_CHECK(out.rgba == source.rgba);
  }

  void testPaddingAndGradient() {
    auto backdrop = plainBackdrop();
    backdrop.gradient = true;
    backdrop.padding = 20;
    backdrop.gradientAngleDegrees = 90.0F; // left (red) to right (blue)
    const auto out = capture::applyScreenshotBackdrop(solidImage(10, 10, 0, 255, 0), backdrop);
    TEST_CHECK(out.width == 50 && out.height == 50);

    const Pixel left = pixelAt(out, 0, 25);
    TEST_CHECK(left.r > 240 && left.b < 15 && left.a == 255);
    const Pixel right = pixelAt(out, out.width - 1, 25);
    TEST_CHECK(right.b > 240 && right.r < 15 && right.a == 255);
    const Pixel centre = pixelAt(out, 25, 25);
    TEST_CHECK(centre.g == 255 && centre.r == 0 && centre.b == 0);
  }

  void testRoundedCorners() {
    auto backdrop = plainBackdrop();
    backdrop.cornerRadius = 8;
    const auto out = capture::applyScreenshotBackdrop(solidImage(32, 32, 255, 255, 255), backdrop);
    TEST_CHECK(pixelAt(out, 0, 0).a == 0);
    TEST_CHECK(pixelAt(out, 16, 0).a == 255);
    TEST_CHECK(pixelAt(out, 16, 16).a == 255);
  }

  void testShadow() {
    auto backdrop = plainBackdrop();
    backdrop.padding = 40;
    backdrop.shadow = true;
    backdrop.shadowOpacity = 1.0F;
    backdrop.shadowBlur = 10;
    backdrop.shadowOffsetY = 12;
    const auto out = capture::applyScreenshotBackdrop(solidImage(20, 20, 255, 255, 255), backdrop);

    const Pixel below = pixelAt(out, 50, 64); // under the capture, inside the offset shadow
    TEST_CHECK(below.a > 100 && near(below.r, 0) && near(below.b, 0));
    TEST_CHECK(pixelAt(out, 50, 30).a < below.a); // above the capture, away from the offset
    TEST_CHECK(pixelAt(out, 0, 0).a == 0);
  }

  void testEmptySource() {
    const auto out = capture::applyScreenshotBackdrop(ScreencopyImage{}, plainBackdrop());
    TEST_CHECK(out.width == 0 && out.rgba.empty());
  }

} // namespace

int main() {
  testPassThrough();
  testPaddingAndGradient();
  testRoundedCorners();
  testShadow();
  testEmptySource();
  return 0;
}
