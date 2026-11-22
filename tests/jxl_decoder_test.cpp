#include "render/codecs/codecs.h"
#include "tests/test_check.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <print>
#include <string>

namespace {

  void checkDecoded(const std::expected<DecodedRasterImage, std::string>& result) {
    if (!result) {
      std::println(stderr, "decode error: {}", result.error());
    }
    TEST_CHECK(result.has_value());
  }

  // Bare codestream (magic FF 0A), 100 bytes.
  constexpr std::array<std::uint8_t, 100> kJxl4x4Bare = {
      0xFF, 0x0A, 0x18, 0x10, 0xB0, 0x12, 0x08, 0x10, 0x10, 0x00, 0x60, 0x01, 0x4B, 0x28, 0x24, 0xC6, 0x15,
      0x0A, 0x0A, 0x12, 0x4E, 0x60, 0x4C, 0x19, 0x7B, 0x62, 0x72, 0x01, 0x81, 0x00, 0x24, 0xE7, 0x87, 0x96,
      0x2F, 0xBF, 0x63, 0x00, 0xF7, 0x27, 0x80, 0xF3, 0x53, 0x3F, 0xF9, 0x23, 0x39, 0x92, 0x24, 0x39, 0xF7,
      0x27, 0x00, 0xEA, 0x27, 0x7F, 0x40, 0x72, 0x24, 0x49, 0x3A, 0xF6, 0xFD, 0xF3, 0xAF, 0x1F, 0xF2, 0xAF,
      0x3F, 0xFF, 0xFA, 0xF3, 0xDF, 0x9D, 0x7F, 0xFD, 0xBB, 0x77, 0x1F, 0x3C, 0x00, 0x02, 0xE0, 0xFC, 0xE4,
      0x4F, 0xFE, 0x48, 0x1F, 0x93, 0xF1, 0xF3, 0x8C, 0xDD, 0x09, 0x13, 0x61, 0xC6, 0xF3, 0x05,
  };

  // Same codestream in an ISOBMFF container (magic 00 00 00 0C 4A 58 4C 20 0D 0A 87 0A), 140 bytes.
  constexpr std::array<std::uint8_t, 140> kJxl4x4Container = {
      0x00, 0x00, 0x00, 0x0C, 0x4A, 0x58, 0x4C, 0x20, 0x0D, 0x0A, 0x87, 0x0A, 0x00, 0x00, 0x00, 0x14, 0x66, 0x74,
      0x79, 0x70, 0x6A, 0x78, 0x6C, 0x20, 0x00, 0x00, 0x00, 0x00, 0x6A, 0x78, 0x6C, 0x20, 0x00, 0x00, 0x00, 0x6C,
      0x6A, 0x78, 0x6C, 0x63, 0xFF, 0x0A, 0x18, 0x10, 0xB0, 0x12, 0x08, 0x10, 0x10, 0x00, 0x60, 0x01, 0x4B, 0x28,
      0x24, 0xC6, 0x15, 0x0A, 0x0A, 0x12, 0x4E, 0x60, 0x4C, 0x19, 0x7B, 0x62, 0x72, 0x01, 0x81, 0x00, 0x24, 0xE7,
      0x87, 0x96, 0x2F, 0xBF, 0x63, 0x00, 0xF7, 0x27, 0x80, 0xF3, 0x53, 0x3F, 0xF9, 0x23, 0x39, 0x92, 0x24, 0x39,
      0xF7, 0x27, 0x00, 0xEA, 0x27, 0x7F, 0x40, 0x72, 0x24, 0x49, 0x3A, 0xF6, 0xFD, 0xF3, 0xAF, 0x1F, 0xF2, 0xAF,
      0x3F, 0xFF, 0xFA, 0xF3, 0xDF, 0x9D, 0x7F, 0xFD, 0xBB, 0x77, 0x1F, 0x3C, 0x00, 0x02, 0xE0, 0xFC, 0xE4, 0x4F,
      0xFE, 0x48, 0x1F, 0x93, 0xF1, 0xF3, 0x8C, 0xDD, 0x09, 0x13, 0x61, 0xC6, 0xF3, 0x05,
  };

  // Row-major RGBA, 16 pixels. Row 3 is half-alpha, row 4 is fully transparent
  // with non-zero RGB — that row is the non-premultiplied-alpha assertion.
  constexpr std::array<std::uint8_t, 64> kExpectedRgba = {
      255, 0,   0,   255, 0,   255, 0,  255, 0,  0,   255, 255, 255, 255, 0,  255, 255, 0,   255, 255, 0,   255,
      255, 255, 255, 255, 255, 255, 0,  0,   0,  255, 128, 0,   0,   128, 0,  128, 0,   128, 0,   0,   128, 128,
      128, 128, 0,   128, 10,  20,  30, 0,   40, 50,  60,  0,   70,  80,  90, 0,   100, 110, 120, 0,
  };

  void checkPixels(const DecodedRasterImage& decoded, const char* label) {
    TEST_CHECK(decoded.width == 4);
    TEST_CHECK(decoded.height == 4);
    TEST_CHECK(decoded.pixels.size() == kExpectedRgba.size());
    for (std::size_t i = 0; i < kExpectedRgba.size(); ++i) {
      if (decoded.pixels[i] != kExpectedRgba[i]) {
        std::println(
            stderr, "{}: byte {} is {}, expected {}", label, i, static_cast<unsigned>(decoded.pixels[i]),
            static_cast<unsigned>(kExpectedRgba[i])
        );
      }
      TEST_CHECK(decoded.pixels[i] == kExpectedRgba[i]);
    }
  }

  void testBareCodestream() {
    auto result = codecs::decodeRasterImage(kJxl4x4Bare.data(), kJxl4x4Bare.size());
    checkDecoded(result);
    checkPixels(*result, "bare codestream");
  }

  void testContainer() {
    auto result = codecs::decodeRasterImage(kJxl4x4Container.data(), kJxl4x4Container.size());
    checkDecoded(result);
    checkPixels(*result, "container");
  }

  void testStraightAlphaPreserved() {
    auto result = codecs::decodeRasterImage(kJxl4x4Bare.data(), kJxl4x4Bare.size());
    checkDecoded(result);
    TEST_CHECK(result->pixels.size() == 64);

    // Fully transparent row: a decoder that zeroed unused color would drop these.
    constexpr std::array<std::uint8_t, 16> kTransparentRow = {
        10, 20, 30, 0, 40, 50, 60, 0, 70, 80, 90, 0, 100, 110, 120, 0,
    };
    for (std::size_t i = 0; i < kTransparentRow.size(); ++i) {
      TEST_CHECK(result->pixels[48 + i] == kTransparentRow[i]);
    }
  }

  // 4x4 lossless codestream whose alpha channel is flagged premultiplied
  // (JxlExtraChannelInfo::alpha_premultiplied), 88 bytes. Stored colors are
  // 0 or equal to alpha, so unpremultiplying is exact in 8 bits.
  constexpr std::array<std::uint8_t, 88> kJxl4x4Premultiplied = {
      0xFF, 0x0A, 0x18, 0x10, 0x30, 0x00, 0x4A, 0x08, 0x10, 0x10, 0x00, 0x2C, 0x01, 0x4B, 0x18, 0x93, 0x8E, 0x85,
      0x83, 0x99, 0x22, 0xA4, 0x3B, 0x01, 0xDD, 0x99, 0x16, 0x12, 0x3C, 0x50, 0x43, 0xE2, 0xBD, 0x80, 0x40, 0x00,
      0x1C, 0xB0, 0xDF, 0x01, 0x07, 0xFC, 0x2F, 0x80, 0xFB, 0xD5, 0x2F, 0x7F, 0xEC, 0x77, 0x80, 0xFF, 0x05, 0xA0,
      0x7E, 0xF9, 0x83, 0x03, 0x0E, 0xD8, 0x4F, 0xFE, 0xF5, 0x93, 0x7F, 0xFD, 0xF9, 0xD7, 0x9F, 0xFF, 0x01, 0xF9,
      0xD5, 0x87, 0x00, 0x04, 0xE0, 0x7E, 0xF9, 0xCB, 0x5F, 0x3A, 0x81, 0xBE, 0xA5, 0xA3, 0xE4, 0x00,
  };

  // The pipeline needs straight alpha. Dropping JxlDecoderSetUnpremultiplyAlpha
  // makes the two translucent rows decode as 128/64 instead of 255.
  void testPremultipliedAlphaIsUnpremultiplied() {
    auto result = codecs::decodeRasterImage(kJxl4x4Premultiplied.data(), kJxl4x4Premultiplied.size());
    checkDecoded(result);
    TEST_CHECK(result->pixels.size() == 64);

    constexpr std::array<std::uint8_t, 64> kExpectedStraight = {
        255, 0,   0,   255, 0,   255, 0,   255, 0, 0,   255, 255, 255, 255, 0,   255, 255, 0,   255, 255, 0,   255,
        255, 255, 255, 255, 255, 255, 0,   0,   0, 255, 255, 0,   0,   128, 0,   255, 0,   128, 0,   0,   255, 128,
        255, 255, 0,   128, 255, 0,   255, 64,  0, 255, 255, 64,  255, 255, 255, 64,  0,   0,   0,   64,
    };
    for (std::size_t i = 0; i < kExpectedStraight.size(); ++i) {
      if (result->pixels[i] != kExpectedStraight[i]) {
        std::println(
            stderr, "premultiplied: byte {} is {}, expected {}", i, static_cast<unsigned>(result->pixels[i]),
            static_cast<unsigned>(kExpectedStraight[i])
        );
      }
      TEST_CHECK(result->pixels[i] == kExpectedStraight[i]);
    }
  }

  void testTruncated() {
    auto result = codecs::decodeRasterImage(kJxl4x4Bare.data(), 40);
    TEST_CHECK(!result.has_value());
  }

  void testTooShortForSignature() {
    constexpr std::array<std::uint8_t, 3> kTooShort = {0x00, 0x00, 0x00};
    auto result = codecs::decodeRasterImage(kTooShort.data(), kTooShort.size());
    TEST_CHECK(!result.has_value());
  }

} // namespace

int main() {
  testBareCodestream();
  testContainer();
  testStraightAlphaPreserved();
  testPremultipliedAlphaIsUnpremultiplied();
  testTruncated();
  testTooShortForSignature();
  return 0;
}
