#include "render/codecs/codecs.h"
#include "render/codecs/color_profile.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <lcms2.h>
#include <print>
#include <span>
#include <string_view>
#include <vector>

namespace {

  bool check(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "color_profile_test: FAIL: {}", message);
    }
    return condition;
  }

  bool near(const std::vector<std::uint8_t>& rgba, std::array<int, 4> expected, int tolerance) {
    for (std::size_t i = 0; i < expected.size(); ++i) {
      if (std::abs(static_cast<int>(rgba[i]) - expected[i]) > tolerance) {
        return false;
      }
    }
    return true;
  }

  std::vector<std::uint8_t> saveProfile(cmsHPROFILE profile) {
    cmsUInt32Number size = 0;
    cmsSaveProfileToMem(profile, nullptr, &size);
    std::vector<std::uint8_t> icc(size);
    cmsSaveProfileToMem(profile, icc.data(), &size);
    cmsCloseProfile(profile);
    return icc;
  }

  // Display P3: P3 primaries, D65 white, the sRGB transfer curve.
  std::vector<std::uint8_t> displayP3Icc() {
    const cmsCIExyY white{0.3127, 0.3290, 1.0};
    const cmsCIExyYTRIPLE primaries{{0.680, 0.320, 1.0}, {0.265, 0.690, 1.0}, {0.150, 0.060, 1.0}};
    const std::array<cmsFloat64Number, 5> srgbCurve{2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
    cmsToneCurve* curve = cmsBuildParametricToneCurve(nullptr, 4, srgbCurve.data());
    std::array<cmsToneCurve*, 3> curves{curve, curve, curve};
    auto icc = saveProfile(cmsCreateRGBProfile(&white, &primaries, curves.data()));
    cmsFreeToneCurve(curve);
    return icc;
  }

  // Linear sRGB: sRGB's primaries and white with a linear transfer curve.
  std::vector<std::uint8_t> linearSrgbIcc() {
    const cmsCIExyY white{0.3127, 0.3290, 1.0};
    const cmsCIExyYTRIPLE primaries{{0.640, 0.330, 1.0}, {0.300, 0.600, 1.0}, {0.150, 0.060, 1.0}};
    cmsToneCurve* curve = cmsBuildGamma(nullptr, 1.0);
    std::array<cmsToneCurve*, 3> curves{curve, curve, curve};
    auto icc = saveProfile(cmsCreateRGBProfile(&white, &primaries, curves.data()));
    cmsFreeToneCurve(curve);
    return icc;
  }

  // One JPEG marker segment: the marker, the big-endian length (which counts itself), the payload.
  void appendSegment(std::vector<std::uint8_t>& jpeg, std::uint8_t marker, const std::vector<std::uint8_t>& payload) {
    const std::size_t length = payload.size() + 2;
    jpeg.insert(jpeg.end(), {0xFF, marker, static_cast<std::uint8_t>(length >> 8), static_cast<std::uint8_t>(length)});
    jpeg.insert(jpeg.end(), payload.begin(), payload.end());
  }

  // The payload of an APP2 ICC_PROFILE segment: the signature, the part's number, the part count, the data.
  std::vector<std::uint8_t> iccPart(std::uint8_t number, std::uint8_t count, const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> payload{'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0, number, count};
    payload.insert(payload.end(), data.begin(), data.end());
    return payload;
  }

  void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
  }

  // A zlib stream holding `data` in one stored (uncompressed) block, which takes up to 65535 bytes.
  std::vector<std::uint8_t> zlibStored(std::span<const std::uint8_t> data) {
    const auto length = static_cast<std::uint16_t>(data.size());
    const auto inverse = static_cast<std::uint16_t>(~length);
    std::vector<std::uint8_t> out{0x78, 0x01, 0x01};
    out.push_back(static_cast<std::uint8_t>(length & 0xFF));
    out.push_back(static_cast<std::uint8_t>(length >> 8));
    out.push_back(static_cast<std::uint8_t>(inverse & 0xFF));
    out.push_back(static_cast<std::uint8_t>(inverse >> 8));
    out.insert(out.end(), data.begin(), data.end());
    // The Adler-32 of the data closes the stream.
    std::uint32_t low = 1;
    std::uint32_t high = 0;
    for (const std::uint8_t byte : data) {
      low = (low + byte) % 65521;
      high = (high + low) % 65521;
    }
    appendU32(out, (high << 16) | low);
    return out;
  }

  // One PNG chunk: the payload's length, the type, the payload, the CRC-32 of type and payload.
  void appendChunk(std::vector<std::uint8_t>& png, std::string_view type, std::span<const std::uint8_t> payload) {
    appendU32(png, static_cast<std::uint32_t>(payload.size()));
    const std::size_t start = png.size();
    png.insert(png.end(), type.begin(), type.end());
    png.insert(png.end(), payload.begin(), payload.end());
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = start; i < png.size(); ++i) {
      crc ^= png[i];
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc >> 1) ^ ((crc & 1U) != 0 ? 0xEDB88320U : 0U);
      }
    }
    appendU32(png, ~crc);
  }

  // The payload of an iCCP chunk: the profile's name, the compression method (0), the deflated profile.
  std::vector<std::uint8_t> iccpPayload(std::span<const std::uint8_t> deflated) {
    std::vector<std::uint8_t> payload{'i', 'c', 'c', 0, 0};
    payload.insert(payload.end(), deflated.begin(), deflated.end());
    return payload;
  }

  // A 1x1 RGB PNG of the pixel (200, 100, 50), with `iccp` as the payload of its iCCP chunk.
  std::vector<std::uint8_t> pngWithProfileChunk(std::span<const std::uint8_t> iccp) {
    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    // Width 1, height 1, 8 bits per channel, color type 2 (RGB).
    constexpr std::array<std::uint8_t, 13> kHeader{0, 0, 0, 1, 0, 0, 0, 1, 8, 2, 0, 0, 0};
    // One row: filter type 0, then the pixel.
    constexpr std::array<std::uint8_t, 4> kRow{0, 200, 100, 50};
    appendChunk(png, "IHDR", kHeader);
    appendChunk(png, "iCCP", iccp);
    appendChunk(png, "IDAT", zlibStored(kRow));
    appendChunk(png, "IEND", {});
    return png;
  }

  // 2x1 lossless JPEG XL codestreams of the pixel (200, 100, 50), tagged BT.2100 PQ and BT.2100 HLG.
  constexpr std::array<std::uint8_t, 38> kJxlPq = {
      0xFF, 0x0A, 0x00, 0x70, 0x01, 0x04, 0xE4, 0xD1, 0x23, 0x0E, 0x07, 0x00, 0x00, 0x00, 0x80, 0x08, 0x00, 0x01, 0x00,
      0x44, 0x00, 0x4B, 0x18, 0x8B, 0x15, 0xC2, 0x09, 0x6C, 0x5C, 0x48, 0xFB, 0x00, 0x7B, 0x23, 0x29, 0xAA, 0xB3, 0x00,
  };
  constexpr std::array<std::uint8_t, 39> kJxlHlg = {
      0xFF, 0x0A, 0x00, 0x70, 0x01, 0x04, 0xE4, 0x19, 0x08, 0xF4, 0x18, 0x00, 0x00,
      0x00, 0x00, 0x02, 0x08, 0x00, 0x01, 0x00, 0x44, 0x00, 0x4B, 0x18, 0x8B, 0x15,
      0xC2, 0x09, 0x6C, 0x5C, 0x48, 0xFB, 0x00, 0x7B, 0x23, 0x29, 0xAA, 0xB3, 0x00,
  };

  // P3 (200, 100, 50) is (215, 93, 31) in sRGB, worked out from the primaries.
  constexpr std::array<int, 4> kP3PixelInSrgb = {215, 93, 31, 77};

  bool testIccConvertsToSrgb() {
    std::vector<std::uint8_t> pixel{200, 100, 50, 77};
    codecs::convertIccToSrgb(pixel, displayP3Icc());
    return check(near(pixel, kP3PixelInSrgb, 2), "a Display P3 ICC profile should convert to sRGB and keep alpha");
  }

  bool testCicpConvertsToSrgb() {
    std::vector<std::uint8_t> pixel{200, 100, 50, 77};
    codecs::convertCicpToSrgb(pixel, 12, 13);
    bool ok = check(near(pixel, kP3PixelInSrgb, 2), "nclx Display P3 should convert to sRGB");

    std::vector<std::uint8_t> grey{128, 128, 128, 255};
    codecs::convertCicpToSrgb(grey, 12, 13);
    ok = check(near(grey, {128, 128, 128, 255}, 1), "a neutral should stay neutral") && ok;
    return ok;
  }

  bool testLinearSrgbConverts() {
    // Linear 128 is sRGB 188.
    std::vector<std::uint8_t> pixel{128, 128, 128, 255};
    codecs::convertIccToSrgb(pixel, linearSrgbIcc());
    bool ok = check(near(pixel, {188, 188, 188, 255}, 1), "a linear profile with sRGB primaries should convert");

    std::vector<std::uint8_t> nclx{128, 128, 128, 255};
    codecs::convertCicpToSrgb(nclx, 1, 8);
    ok = check(near(nclx, {188, 188, 188, 255}, 1), "nclx sRGB primaries with a linear curve should convert") && ok;
    return ok;
  }

  bool testUnchangedWithoutConversion() {
    const std::vector<std::uint8_t> original{200, 100, 50, 255};
    bool ok = true;

    auto pixel = original;
    codecs::convertIccToSrgb(pixel, saveProfile(cmsCreate_sRGBProfile()));
    ok = check(pixel == original, "an sRGB profile should change nothing") && ok;

    codecs::convertCicpToSrgb(pixel, 1, 13);
    ok = check(pixel == original, "BT.709 primaries should change nothing") && ok;

    codecs::convertCicpToSrgb(pixel, 1, 1);
    ok = check(pixel == original, "BT.709 primaries with the BT.709 curve should change nothing") && ok;

    codecs::convertCicpToSrgb(pixel, 9, 16);
    ok = check(pixel == original, "a PQ transfer should be left alone") && ok;

    constexpr std::array<std::uint8_t, 4> kGarbage = {1, 2, 3, 4};
    codecs::convertIccToSrgb(pixel, kGarbage);
    ok = check(pixel == original, "an unreadable profile should change nothing") && ok;
    return ok;
  }

  bool testJpegProfileReassembled() {
    // Start of image, then an APP0 segment before the profile.
    std::vector<std::uint8_t> jpeg{0xFF, 0xD8};
    appendSegment(jpeg, 0xE0, {'J', 'F'});
    // The profile's two parts, stored out of order; cutting the file after the first leaves one part.
    appendSegment(jpeg, 0xE2, iccPart(2, 2, {'c', 'd', 'e'}));
    const std::size_t onePartSize = jpeg.size();
    appendSegment(jpeg, 0xE2, iccPart(1, 2, {'a', 'b'}));
    // Start of scan: no metadata follows it.
    appendSegment(jpeg, 0xDA, {});

    const std::vector<std::uint8_t> expected{'a', 'b', 'c', 'd', 'e'};
    const auto whole = codecs::jpegIccProfile(jpeg.data(), jpeg.size());
    const auto cut = codecs::jpegIccProfile(jpeg.data(), onePartSize);
    bool ok = check(whole == expected, "both parts should be joined in order");
    ok = check(cut.empty(), "a profile missing a part should be ignored") && ok;
    return ok;
  }

  bool testPngProfileConverts() {
    const auto png = pngWithProfileChunk(iccpPayload(zlibStored(displayP3Icc())));
    const auto decoded = codecs::decodeRasterImage(png.data(), png.size());
    return check(
        decoded && near(decoded->pixels, {215, 93, 31, 255}, 2), "a PNG's Display P3 profile should convert its pixels"
    );
  }

  bool testPngWithBrokenProfileDecodes() {
    const auto deflated = zlibStored(displayP3Icc());
    constexpr std::array<std::uint8_t, 8> kNotDeflate = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::vector<std::uint8_t> original{200, 100, 50, 255};
    bool ok = true;

    auto png = pngWithProfileChunk(iccpPayload(kNotDeflate));
    auto decoded = codecs::decodeRasterImage(png.data(), png.size());
    ok = check(decoded && decoded->pixels == original, "a profile that is not deflate data should be ignored") && ok;

    png = pngWithProfileChunk(iccpPayload(std::span(deflated).first(deflated.size() / 2)));
    decoded = codecs::decodeRasterImage(png.data(), png.size());
    ok = check(decoded && decoded->pixels == original, "a profile cut short should be ignored") && ok;

    // No terminator after the profile's name.
    png = pngWithProfileChunk(std::vector<std::uint8_t>(100, 'A'));
    decoded = codecs::decodeRasterImage(png.data(), png.size());
    ok = check(decoded && decoded->pixels == original, "a profile chunk without a name should be ignored") && ok;
    return ok;
  }

  bool testHdrJxlLeftAlone() {
    const std::vector<std::uint8_t> original{200, 100, 50, 255, 200, 100, 50, 255};
    auto decoded = codecs::decodeRasterImage(kJxlPq.data(), kJxlPq.size());
    bool ok = check(decoded && decoded->pixels == original, "a PQ JPEG XL should be left alone");

    decoded = codecs::decodeRasterImage(kJxlHlg.data(), kJxlHlg.size());
    ok = check(decoded && decoded->pixels == original, "an HLG JPEG XL should be left alone") && ok;
    return ok;
  }

} // namespace

int main() {
  bool ok = true;
  ok = testIccConvertsToSrgb() && ok;
  ok = testCicpConvertsToSrgb() && ok;
  ok = testLinearSrgbConverts() && ok;
  ok = testUnchangedWithoutConversion() && ok;
  ok = testJpegProfileReassembled() && ok;
  ok = testPngProfileConverts() && ok;
  ok = testPngWithBrokenProfileDecodes() && ok;
  ok = testHdrJxlLeftAlone() && ok;
  return ok ? 0 : 1;
}
