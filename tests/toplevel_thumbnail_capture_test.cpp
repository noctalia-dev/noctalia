#include "capture/toplevel_thumbnail_capture.h"
#include "tests/test_check.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <wayland-client-protocol.h>

namespace {
  std::array<std::uint8_t, 4> bytes(std::uint32_t pixel) {
    std::array<std::uint8_t, 4> result{};
    std::memcpy(result.data(), &pixel, sizeof(pixel));
    return result;
  }

  void decodesSharedMemoryFormats() {
    const auto argbRed = bytes(0xFFFF0000U);
    const auto argb =
        capture::makeToplevelThumbnail(argbRed, 1, 1, WL_SHM_FORMAT_ARGB8888, 8, 8, WL_OUTPUT_TRANSFORM_NORMAL);
    TEST_CHECK(argb.has_value());
    TEST_CHECK(argb->rgba == std::vector<std::uint8_t>({255, 0, 0, 255}));

    const auto abgrRed = bytes(0xFF0000FFU);
    const auto abgr =
        capture::makeToplevelThumbnail(abgrRed, 1, 1, WL_SHM_FORMAT_ABGR8888, 8, 8, WL_OUTPUT_TRANSFORM_NORMAL);
    TEST_CHECK(abgr.has_value());
    TEST_CHECK(abgr->rgba == std::vector<std::uint8_t>({255, 0, 0, 255}));

    const auto premultiplied = bytes(0x80400000U);
    const auto straight =
        capture::makeToplevelThumbnail(premultiplied, 1, 1, WL_SHM_FORMAT_ARGB8888, 8, 8, WL_OUTPUT_TRANSFORM_NORMAL);
    TEST_CHECK(straight.has_value());
    TEST_CHECK(straight->rgba[0] == 128 && straight->rgba[3] == 128);
  }

  void boundsAndOrientsThumbnail() {
    std::array<std::uint8_t, 32> uniform{};
    for (std::size_t i = 0; i < uniform.size(); i += 4) {
      uniform[i] = 12;
      uniform[i + 1] = 34;
      uniform[i + 2] = 56;
      uniform[i + 3] = 255;
    }
    const auto bounded =
        capture::makeToplevelThumbnail(uniform, 4, 2, WL_SHM_FORMAT_ABGR8888, 2, 2, WL_OUTPUT_TRANSFORM_NORMAL);
    TEST_CHECK(bounded.has_value());
    TEST_CHECK(bounded->width == 2 && bounded->height == 1);
    TEST_CHECK(bounded->rgba.size() == 8);

    const auto red = bytes(0xFF0000FFU);
    const auto green = bytes(0xFF00FF00U);
    std::array<std::uint8_t, 8> row{};
    std::ranges::copy(red, row.begin());
    std::ranges::copy(green, row.begin() + 4);
    const auto rotated =
        capture::makeToplevelThumbnail(row, 2, 1, WL_SHM_FORMAT_ABGR8888, 8, 8, WL_OUTPUT_TRANSFORM_90);
    TEST_CHECK(rotated.has_value());
    TEST_CHECK(rotated->width == 1 && rotated->height == 2);
    TEST_CHECK(rotated->rgba[0] == 255 && rotated->rgba[1] == 0 && rotated->rgba[4] == 0 && rotated->rgba[5] == 255);

    std::array<std::uint8_t, 32> portraitBuffer{};
    const auto rotatedBounded =
        capture::makeToplevelThumbnail(portraitBuffer, 4, 2, WL_SHM_FORMAT_ABGR8888, 2, 3, WL_OUTPUT_TRANSFORM_90);
    TEST_CHECK(rotatedBounded.has_value());
    TEST_CHECK(rotatedBounded->width <= 2 && rotatedBounded->height <= 3);
  }

  void rejectsInvalidBuffers() {
    const std::array<std::uint8_t, 3> shortBuffer{};
    TEST_CHECK(
        !capture::makeToplevelThumbnail(shortBuffer, 1, 1, WL_SHM_FORMAT_ABGR8888, 8, 8, WL_OUTPUT_TRANSFORM_NORMAL)
             .has_value()
    );
  }

  void keepsPixelsExactAtIdentityScale() {
    std::array<std::uint8_t, 16> pixels{};
    const std::array<std::uint32_t, 4> source{0xFF0000FFU, 0x80000080U, 0xFF00FF00U, 0x40201008U};
    for (std::size_t i = 0; i < source.size(); ++i) {
      std::memcpy(pixels.data() + i * 4U, &source[i], sizeof(std::uint32_t));
    }
    const auto native = capture::makeToplevelThumbnail(
        pixels, 2, 2, WL_SHM_FORMAT_ARGB8888, 1 << 30, 1 << 30, WL_OUTPUT_TRANSFORM_NORMAL
    );
    TEST_CHECK(native.has_value());
    TEST_CHECK(native->width == 2 && native->height == 2);
    // Un-premultiplied straight RGBA, byte order R,G,B,A per pixel.
    const std::vector<std::uint8_t> expected{
        0,   0,   255, 255, // 0xFF0000FF
        0,   0,   255, 128, // 0x80000080
        0,   255, 0,   255, // 0xFF00FF00
        128, 64,  32,  64   // 0x40201008
    };
    TEST_CHECK(native->rgba == expected);
    const auto bounded =
        capture::makeToplevelThumbnail(pixels, 2, 2, WL_SHM_FORMAT_ARGB8888, 8, 8, WL_OUTPUT_TRANSFORM_NORMAL);
    TEST_CHECK(bounded.has_value() && bounded->rgba == native->rgba);
  }
} // namespace

int main() {
  decodesSharedMemoryFormats();
  boundsAndOrientsThumbnail();
  rejectsInvalidBuffers();
  keepsPixelsExactAtIdentityScale();
  return 0;
}
