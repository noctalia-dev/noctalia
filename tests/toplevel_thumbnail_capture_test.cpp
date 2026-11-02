#include "capture/toplevel_thumbnail_capture.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <wayland-client-protocol.h>

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
  }

  std::array<std::uint8_t, 4> bytes(std::uint32_t pixel) {
    std::array<std::uint8_t, 4> result{};
    std::memcpy(result.data(), &pixel, sizeof(pixel));
    return result;
  }

  bool decodesSharedMemoryFormats() {
    bool ok = true;
    const auto argbRed = bytes(0xFFFF0000U);
    const auto argb =
        capture::makeToplevelThumbnail(argbRed, 1, 1, WL_SHM_FORMAT_ARGB8888, 8, 8, WL_OUTPUT_TRANSFORM_NORMAL);
    ok = expect(argb.has_value(), "ARGB thumbnail should decode") && ok;
    if (argb.has_value()) {
      ok = expect(argb->rgba == std::vector<std::uint8_t>({255, 0, 0, 255}), "ARGB channels should become RGBA") && ok;
    }

    const auto abgrRed = bytes(0xFF0000FFU);
    const auto abgr =
        capture::makeToplevelThumbnail(abgrRed, 1, 1, WL_SHM_FORMAT_ABGR8888, 8, 8, WL_OUTPUT_TRANSFORM_NORMAL);
    ok = expect(abgr.has_value(), "ABGR thumbnail should decode") && ok;
    if (abgr.has_value()) {
      ok = expect(abgr->rgba == std::vector<std::uint8_t>({255, 0, 0, 255}), "ABGR channels should become RGBA") && ok;
    }

    const auto premultiplied = bytes(0x80400000U);
    const auto straight =
        capture::makeToplevelThumbnail(premultiplied, 1, 1, WL_SHM_FORMAT_ARGB8888, 8, 8, WL_OUTPUT_TRANSFORM_NORMAL);
    ok = expect(straight.has_value(), "premultiplied ARGB thumbnail should decode") && ok;
    if (straight.has_value()) {
      ok = expect(
               straight->rgba[0] == 128 && straight->rgba[3] == 128,
               "premultiplied color channels should be converted to straight alpha"
           )
          && ok;
    }
    return ok;
  }

  bool boundsAndOrientsThumbnail() {
    bool ok = true;
    std::array<std::uint8_t, 32> uniform{};
    for (std::size_t i = 0; i < uniform.size(); i += 4) {
      uniform[i] = 12;
      uniform[i + 1] = 34;
      uniform[i + 2] = 56;
      uniform[i + 3] = 255;
    }
    const auto bounded =
        capture::makeToplevelThumbnail(uniform, 4, 2, WL_SHM_FORMAT_ABGR8888, 2, 2, WL_OUTPUT_TRANSFORM_NORMAL);
    ok = expect(bounded.has_value(), "bounded thumbnail should decode") && ok;
    if (bounded.has_value()) {
      ok = expect(bounded->width == 2 && bounded->height == 1, "thumbnail should preserve aspect within its bounds")
          && ok;
      ok = expect(bounded->rgba.size() == 8, "bounded thumbnail should allocate only its downsampled pixels") && ok;
    }

    const auto red = bytes(0xFF0000FFU);
    const auto green = bytes(0xFF00FF00U);
    std::array<std::uint8_t, 8> row{};
    std::ranges::copy(red, row.begin());
    std::ranges::copy(green, row.begin() + 4);
    const auto rotated =
        capture::makeToplevelThumbnail(row, 2, 1, WL_SHM_FORMAT_ABGR8888, 8, 8, WL_OUTPUT_TRANSFORM_90);
    ok = expect(rotated.has_value(), "rotated thumbnail should decode") && ok;
    if (rotated.has_value()) {
      ok = expect(rotated->width == 1 && rotated->height == 2, "90-degree transform should swap dimensions") && ok;
      ok = expect(
               rotated->rgba[0] == 255 && rotated->rgba[1] == 0 && rotated->rgba[4] == 0 && rotated->rgba[5] == 255,
               "90-degree transform should preserve pixel order"
           )
          && ok;
    }

    std::array<std::uint8_t, 32> portraitBuffer{};
    const auto rotatedBounded =
        capture::makeToplevelThumbnail(portraitBuffer, 4, 2, WL_SHM_FORMAT_ABGR8888, 2, 3, WL_OUTPUT_TRANSFORM_90);
    ok = expect(rotatedBounded.has_value(), "bounded rotated thumbnail should decode") && ok;
    if (rotatedBounded.has_value()) {
      ok = expect(
               rotatedBounded->width <= 2 && rotatedBounded->height <= 3,
               "thumbnail bounds should apply after orientation"
           )
          && ok;
    }
    return ok;
  }

  bool rejectsInvalidBuffers() {
    const std::array<std::uint8_t, 3> shortBuffer{};
    return expect(
        !capture::makeToplevelThumbnail(shortBuffer, 1, 1, WL_SHM_FORMAT_ABGR8888, 8, 8, WL_OUTPUT_TRANSFORM_NORMAL)
             .has_value(),
        "short source buffer should be rejected"
    );
  }

  bool keepsPixelsExactAtIdentityScale() {
    bool ok = true;
    std::array<std::uint8_t, 16> pixels{};
    const std::array<std::uint32_t, 4> source{0xFF0000FFU, 0x80000080U, 0xFF00FF00U, 0x40201008U};
    for (std::size_t i = 0; i < source.size(); ++i) {
      std::memcpy(pixels.data() + i * 4U, &source[i], sizeof(std::uint32_t));
    }
    const auto native = capture::makeToplevelThumbnail(
        pixels, 2, 2, WL_SHM_FORMAT_ARGB8888, 1 << 30, 1 << 30, WL_OUTPUT_TRANSFORM_NORMAL
    );
    ok = expect(native.has_value(), "identity-scale capture should decode") && ok;
    if (native.has_value()) {
      ok = expect(native->width == 2 && native->height == 2, "identity-scale should keep native dimensions") && ok;
      // Un-premultiplied straight RGBA, byte order R,G,B,A per pixel.
      const std::vector<std::uint8_t> expected{
          0,   0,   255, 255, // 0xFF0000FF
          0,   0,   255, 128, // 0x80000080
          0,   255, 0,   255, // 0xFF00FF00
          128, 64,  32,  64   // 0x40201008
      };
      ok = expect(native->rgba == expected, "identity-scale pixels should match the bounded conversion") && ok;
    }
    const auto bounded =
        capture::makeToplevelThumbnail(pixels, 2, 2, WL_SHM_FORMAT_ARGB8888, 8, 8, WL_OUTPUT_TRANSFORM_NORMAL);
    ok = expect(bounded.has_value() && bounded->rgba == native->rgba, "bounded and native must agree at scale 1.00")
        && ok;
    return ok;
  }
} // namespace

int main() {
  bool ok = true;
  ok = decodesSharedMemoryFormats() && ok;
  ok = boundsAndOrientsThumbnail() && ok;
  ok = rejectsInvalidBuffers() && ok;
  ok = keepsPixelsExactAtIdentityScale() && ok;
  return ok ? 0 : 1;
}
