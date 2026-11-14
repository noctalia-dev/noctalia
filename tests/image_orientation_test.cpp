#include "capture/screencopy_capture.h"
#include "capture/screencopy_util.h"
#include "render/codecs/codecs.h"
#include "render/codecs/image_encoder.h"
#include "render/core/image_orientation.h"
#include "tests/test_check.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <print>
#include <vector>
#include <wayland-client-protocol.h>

namespace {

  // 3x2 source; every pixel's red channel encodes x + 10*y.
  std::vector<std::uint8_t> sourceImage() {
    std::vector<std::uint8_t> rgba;
    rgba.reserve(3U * 2U * 4U);
    for (int y = 0; y < 2; ++y) {
      for (int x = 0; x < 3; ++x) {
        rgba.push_back(static_cast<std::uint8_t>(x + (10 * y)));
        rgba.push_back(0x00);
        rgba.push_back(0x00);
        rgba.push_back(0xFF);
      }
    }
    return rgba;
  }

  std::vector<std::uint8_t> redChannel(const std::vector<std::uint8_t>& rgba) {
    std::vector<std::uint8_t> reds;
    reds.reserve(rgba.size() / 4U);
    for (std::size_t i = 0; i < rgba.size(); i += 4U) {
      reds.push_back(rgba[i]);
    }
    return reds;
  }

  struct TransformCase {
    ImageOrientation orientation;
    const char* name;
    int width;
    int height;
    std::vector<std::uint8_t> reds;
  };

  void checkTransforms() {
    const std::vector<TransformCase> cases{
        {ImageOrientation::Normal, "normal", 3, 2, {0, 1, 2, 10, 11, 12}},
        {ImageOrientation::MirrorHorizontal, "mirror-horizontal", 3, 2, {2, 1, 0, 12, 11, 10}},
        {ImageOrientation::Rotate180, "rotate-180", 3, 2, {12, 11, 10, 2, 1, 0}},
        {ImageOrientation::MirrorVertical, "mirror-vertical", 3, 2, {10, 11, 12, 0, 1, 2}},
        {ImageOrientation::Transpose, "transpose", 2, 3, {0, 10, 1, 11, 2, 12}},
        {ImageOrientation::Rotate90, "rotate-90", 2, 3, {10, 0, 11, 1, 12, 2}},
        {ImageOrientation::Transverse, "transverse", 2, 3, {12, 2, 11, 1, 10, 0}},
        {ImageOrientation::Rotate270, "rotate-270", 2, 3, {2, 12, 1, 11, 0, 10}},
    };

    for (const TransformCase& testCase : cases) {
      std::vector<std::uint8_t> rgba = sourceImage();
      int width = 3;
      int height = 2;
      applyImageOrientation(rgba, width, height, testCase.orientation);
      const bool sizeMatches = width == testCase.width && height == testCase.height;
      if (!sizeMatches) {
        std::println(stderr, "{}: wrong size {}x{}", testCase.name, width, height);
      }
      TEST_CHECK(sizeMatches);
      const bool pixelsMatch = redChannel(rgba) == testCase.reds;
      if (!pixelsMatch) {
        std::println(stderr, "{}: wrong pixel order", testCase.name);
      }
      TEST_CHECK(pixelsMatch);
    }
  }

  void checkScreencopyOrientation() {
    ScreencopyImage image{
        .width = 3,
        .height = 2,
        .yInvert = true,
        .rgba = sourceImage(),
    };

    screencopy::orientCaptureForTransform(image, WL_OUTPUT_TRANSFORM_90);

    TEST_CHECK(image.width == 2 && image.height == 3);
    TEST_CHECK(!image.yInvert);
    // y-invert must apply before the output transform.
    TEST_CHECK(redChannel(image.rgba) == std::vector<std::uint8_t>{0, 10, 1, 11, 2, 12});
  }

  std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < size; ++i) {
      crc ^= data[i];
      for (int bit = 0; bit < 8; ++bit) {
        crc = ((crc & 1U) != 0U) ? (crc >> 1U) ^ 0xEDB88320U : crc >> 1U;
      }
    }
    return ~crc;
  }

  void appendU32BE(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24U));
    out.push_back(static_cast<std::uint8_t>(value >> 16U));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
    out.push_back(static_cast<std::uint8_t>(value));
  }

  // Little-endian TIFF block with one IFD0 entry: Orientation = value.
  std::vector<std::uint8_t> tiffOrientationBlock(std::uint16_t value) {
    std::vector<std::uint8_t> tiff{'I', 'I', 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00};
    tiff.insert(tiff.end(), {0x01, 0x00});             // entry count
    tiff.insert(tiff.end(), {0x12, 0x01});             // tag 0x0112
    tiff.insert(tiff.end(), {0x03, 0x00});             // type SHORT
    tiff.insert(tiff.end(), {0x01, 0x00, 0x00, 0x00}); // count 1
    tiff.push_back(static_cast<std::uint8_t>(value));
    tiff.insert(tiff.end(), {0x00, 0x00, 0x00});       // value, left-justified
    tiff.insert(tiff.end(), {0x00, 0x00, 0x00, 0x00}); // next IFD offset
    return tiff;
  }

  // Splices an eXIf chunk carrying `orientation` in after the PNG's IHDR.
  std::vector<std::uint8_t> pngWithExifOrientation(std::uint16_t orientation) {
    const std::vector<std::uint8_t> rgba = sourceImage();
    const std::vector<std::uint8_t> png = encodePng(rgba.data(), 3, 2);
    if (png.size() < 33) {
      return {};
    }

    const std::vector<std::uint8_t> tiff = tiffOrientationBlock(orientation);
    std::vector<std::uint8_t> chunk{'e', 'X', 'I', 'f'};
    chunk.insert(chunk.end(), tiff.begin(), tiff.end());

    // 8-byte signature + 25-byte IHDR chunk.
    constexpr std::size_t kAfterIhdr = 8 + 25;
    std::vector<std::uint8_t> out(png.begin(), png.begin() + kAfterIhdr);
    appendU32BE(out, static_cast<std::uint32_t>(tiff.size()));
    out.insert(out.end(), chunk.begin(), chunk.end());
    appendU32BE(out, crc32(chunk.data(), chunk.size()));
    out.insert(out.end(), png.begin() + kAfterIhdr, png.end());
    return out;
  }

  void checkPngDecode() {
    const std::vector<std::uint8_t> upright = pngWithExifOrientation(1);
    TEST_CHECK(!upright.empty());
    const std::vector<std::uint8_t> rotated = pngWithExifOrientation(6);
    TEST_CHECK(!rotated.empty());

    TEST_CHECK(exifOrientation(rotated.data(), rotated.size()) == ImageOrientation::Rotate90);

    const auto decodedUpright = codecs::decodeRasterImage(upright.data(), upright.size());
    TEST_CHECK(decodedUpright.has_value());
    TEST_CHECK(decodedUpright->width == 3 && decodedUpright->height == 2);
    TEST_CHECK(redChannel(decodedUpright->pixels) == std::vector<std::uint8_t>{0, 1, 2, 10, 11, 12});

    const auto decodedRotated = codecs::decodeRasterImage(rotated.data(), rotated.size());
    TEST_CHECK(decodedRotated.has_value());
    TEST_CHECK(decodedRotated->width == 2 && decodedRotated->height == 3);
    TEST_CHECK(redChannel(decodedRotated->pixels) == std::vector<std::uint8_t>{10, 0, 11, 1, 12, 2});
  }

  void checkMalformed() {
    // JPEG SOI followed by a truncated APP1 Exif segment.
    constexpr std::array<std::uint8_t, 10> kTruncatedApp1{0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x20, 'E', 'x', 'i', 'f'};
    TEST_CHECK(exifOrientation(kTruncatedApp1.data(), kTruncatedApp1.size()) == ImageOrientation::Normal);

    constexpr std::array<std::uint8_t, 4> kGarbage{0x01, 0x02, 0x03, 0x04};
    TEST_CHECK(exifOrientation(kGarbage.data(), kGarbage.size()) == ImageOrientation::Normal);
    TEST_CHECK(exifOrientation(nullptr, 0) == ImageOrientation::Normal);

    // An out-of-range tag value must not select a transform.
    const std::vector<std::uint8_t> bogus = pngWithExifOrientation(42);
    TEST_CHECK(exifOrientation(bogus.data(), bogus.size()) == ImageOrientation::Normal);
  }

} // namespace

int main() {
  checkTransforms();
  checkScreencopyOrientation();
  checkPngDecode();
  checkMalformed();
  return 0;
}
