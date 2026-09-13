#include "render/core/image_decoder.h"
#include "render/core/image_encoder.h"
#include "render/core/image_orientation.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <print>
#include <string>
#include <vector>

namespace {

  bool check(bool condition, const std::string& message) {
    if (!condition) {
      std::println(stderr, "image_orientation_test: FAIL: {}", message);
    }
    return condition;
  }

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

  bool checkTransforms() {
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

    bool ok = true;
    for (const TransformCase& testCase : cases) {
      std::vector<std::uint8_t> rgba = sourceImage();
      int width = 3;
      int height = 2;
      applyImageOrientation(rgba, width, height, testCase.orientation);
      ok = check(
               width == testCase.width && height == testCase.height,
               std::string(testCase.name) + ": wrong size " + std::to_string(width) + "x" + std::to_string(height)
           )
          && ok;
      ok = check(redChannel(rgba) == testCase.reds, std::string(testCase.name) + ": wrong pixel order") && ok;
    }
    return ok;
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

  bool checkPngDecode() {
    bool ok = true;

    const std::vector<std::uint8_t> upright = pngWithExifOrientation(1);
    ok = check(!upright.empty(), "failed to build the upright PNG") && ok;
    const std::vector<std::uint8_t> rotated = pngWithExifOrientation(6);
    ok = check(!rotated.empty(), "failed to build the rotated PNG") && ok;
    if (!ok) {
      return false;
    }

    ok = check(
             exifOrientation(rotated.data(), rotated.size()) == ImageOrientation::Rotate90,
             "PNG eXIf orientation was not read"
         )
        && ok;

    const auto decodedUpright = decodeRasterImage(upright.data(), upright.size());
    ok = check(decodedUpright.has_value(), "upright PNG failed to decode") && ok;
    if (decodedUpright) {
      ok = check(decodedUpright->width == 3 && decodedUpright->height == 2, "orientation 1 changed the image size")
          && ok;
      ok = check(
               redChannel(decodedUpright->pixels) == std::vector<std::uint8_t>{0, 1, 2, 10, 11, 12},
               "orientation 1 moved pixels"
           )
          && ok;
    }

    const auto decodedRotated = decodeRasterImage(rotated.data(), rotated.size());
    ok = check(decodedRotated.has_value(), "rotated PNG failed to decode") && ok;
    if (decodedRotated) {
      ok =
          check(decodedRotated->width == 2 && decodedRotated->height == 3, "orientation 6 did not swap the axes") && ok;
      ok = check(
               redChannel(decodedRotated->pixels) == std::vector<std::uint8_t>{10, 0, 11, 1, 12, 2},
               "orientation 6 did not rotate the pixels"
           )
          && ok;
    }

    return ok;
  }

  bool checkMalformed() {
    bool ok = true;

    // JPEG SOI followed by a truncated APP1 Exif segment.
    constexpr std::array<std::uint8_t, 10> kTruncatedApp1{0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x20, 'E', 'x', 'i', 'f'};
    ok = check(
             exifOrientation(kTruncatedApp1.data(), kTruncatedApp1.size()) == ImageOrientation::Normal,
             "a truncated APP1 segment was not rejected"
         )
        && ok;

    constexpr std::array<std::uint8_t, 4> kGarbage{0x01, 0x02, 0x03, 0x04};
    ok = check(
             exifOrientation(kGarbage.data(), kGarbage.size()) == ImageOrientation::Normal,
             "a non-image buffer reported an orientation"
         )
        && ok;
    ok =
        check(exifOrientation(nullptr, 0) == ImageOrientation::Normal, "an empty buffer reported an orientation") && ok;

    // An out-of-range tag value must not select a transform.
    const std::vector<std::uint8_t> bogus = pngWithExifOrientation(42);
    ok = check(
             exifOrientation(bogus.data(), bogus.size()) == ImageOrientation::Normal,
             "an out-of-range orientation value was accepted"
         )
        && ok;

    return ok;
  }

} // namespace

int main() {
  bool ok = checkTransforms();
  ok = checkPngDecode() && ok;
  ok = checkMalformed() && ok;
  return ok ? 0 : 1;
}
