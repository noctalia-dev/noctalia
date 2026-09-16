#include "render/core/image_decoder.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <print>
#include <string>

namespace {

  bool check(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "webp_decoder_test: FAIL: {}", message);
    }
    return condition;
  }

  bool checkDecoded(const std::expected<DecodedRasterImage, std::string>& result, const char* message) {
    if (result) {
      return true;
    }
    const std::string detail = std::string(message) + ": " + result.error();
    return check(false, detail.c_str());
  }

  // 16x16 lossless still, solid opaque red.
  constexpr std::array<std::uint8_t, 36> kStillRed = {
      0x52, 0x49, 0x46, 0x46, 0x1C, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x4C, 0x0F, 0x00,
      0x00, 0x00, 0x2F, 0x0F, 0xC0, 0x03, 0x00, 0x07, 0x10, 0xFD, 0x8F, 0xFE, 0x07, 0x22, 0xA2, 0xFF, 0x01, 0x00,
  };

  // 16x16 lossless VP8X animation: frame 1 solid red, frame 2 solid blue. The still decoder
  // rejects it, so it also pins that the decoder returns the *first* frame and not the last.
  constexpr std::array<std::uint8_t, 140> kAnimatedRedThenBlue = {
      0x52, 0x49, 0x46, 0x46, 0x84, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x58, 0x0A, 0x00,
      0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x41, 0x4E, 0x49, 0x4D, 0x06, 0x00,
      0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x41, 0x4E, 0x4D, 0x46, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x64, 0x00, 0x00, 0x02, 0x56, 0x50, 0x38, 0x4C,
      0x0F, 0x00, 0x00, 0x00, 0x2F, 0x0F, 0xC0, 0x03, 0x00, 0x07, 0x10, 0xFD, 0x8F, 0xFE, 0x07, 0x22, 0xA2, 0xFF,
      0x01, 0x00, 0x41, 0x4E, 0x4D, 0x46, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x00,
      0x00, 0x0F, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x56, 0x50, 0x38, 0x4C, 0x0F, 0x00, 0x00, 0x00, 0x2F, 0x0F,
      0xC0, 0x03, 0x00, 0x07, 0x10, 0xD1, 0xFF, 0xFE, 0x07, 0x22, 0xA2, 0xFF, 0x01, 0x00,
  };

  bool
  checkSolid(const DecodedRasterImage& image, std::uint8_t r, std::uint8_t g, std::uint8_t b, const char* message) {
    if (image.pixels.size() != static_cast<std::size_t>(image.width) * image.height * 4) {
      return check(false, message);
    }
    for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
      if (image.pixels[i] != r || image.pixels[i + 1] != g || image.pixels[i + 2] != b || image.pixels[i + 3] != 0xFF) {
        return check(false, message);
      }
    }
    return true;
  }

} // namespace

int main() {
  bool ok = true;

  const auto still = decodeRasterImage(kStillRed.data(), kStillRed.size());
  ok = checkDecoded(still, "still WebP failed to decode") && ok;
  if (still) {
    ok = check(still->width == 16 && still->height == 16, "still WebP decoded to the wrong size") && ok;
    ok = checkSolid(*still, 0xFF, 0x00, 0x00, "still WebP did not decode to opaque red") && ok;
  }

  const auto animated = decodeRasterImage(kAnimatedRedThenBlue.data(), kAnimatedRedThenBlue.size());
  ok = checkDecoded(animated, "animated WebP failed to decode") && ok;
  if (animated) {
    ok = check(animated->width == 16 && animated->height == 16, "animated WebP decoded to the wrong size") && ok;
    ok = checkSolid(*animated, 0xFF, 0x00, 0x00, "animated WebP did not decode to its first frame") && ok;
  }

  // A truncated animation must fail rather than hand back a half-built canvas.
  const auto truncated = decodeRasterImage(kAnimatedRedThenBlue.data(), 48);
  ok = check(!truncated.has_value(), "truncated animated WebP decoded instead of failing") && ok;

  return ok ? 0 : 1;
}
