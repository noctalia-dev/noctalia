#include "render/core/image_decoder.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <string>
#include <vector>

namespace {

  bool check(bool condition, const char* message) {
    if (!condition) {
      std::fprintf(stderr, "ico_decoder_test: FAIL: %s\n", message);
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

  void writeU16LE(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
  }

  void writeU32LE(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
  }

  // Valid 1x1 red PNG (RGBA), generated with correct CRCs and zlib stream.
  const std::uint8_t kPng1x1[] = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
      0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
      0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
      0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
      0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89, 0x99, 0x3D, 0x1D, 0x00, 0x00,
      0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
  };

  std::vector<std::uint8_t> buildIcoWithPng(int dirWidth, int dirHeight) {
    std::vector<std::uint8_t> ico;

    // ICONDIR header
    writeU16LE(ico, 0);    // reserved
    writeU16LE(ico, 1);    // type = ICO
    writeU16LE(ico, 1);    // count = 1

    // ICONDIRENTRY
    ico.push_back(static_cast<std::uint8_t>(dirWidth == 256 ? 0 : dirWidth));
    ico.push_back(static_cast<std::uint8_t>(dirHeight == 256 ? 0 : dirHeight));
    ico.push_back(0);      // color count
    ico.push_back(0);      // reserved
    writeU16LE(ico, 1);    // planes
    writeU16LE(ico, 32);   // bit count
    writeU32LE(ico, static_cast<std::uint32_t>(sizeof(kPng1x1))); // bytes in res
    writeU32LE(ico, 22);   // image offset (6 + 16 = 22)

    // PNG sub-image
    ico.insert(ico.end(), kPng1x1, kPng1x1 + sizeof(kPng1x1));
    return ico;
  }

  std::vector<std::uint8_t> buildIcoWithBmp(int width, int height, int bpp) {
    std::vector<std::uint8_t> ico;

    const int rowBytes = ((width * bpp + 31) / 32) * 4;
    const int andRowBytes = ((width + 31) / 32) * 4;
    const int pixelDataSize = rowBytes * height;
    const int andMaskSize = andRowBytes * height;
    const int dibHeaderSize = 40;
    const int dibSize = dibHeaderSize + pixelDataSize + andMaskSize;

    // ICONDIR header
    writeU16LE(ico, 0);
    writeU16LE(ico, 1);
    writeU16LE(ico, 1);

    // ICONDIRENTRY
    ico.push_back(static_cast<std::uint8_t>(width == 256 ? 0 : width));
    ico.push_back(static_cast<std::uint8_t>(height == 256 ? 0 : height));
    ico.push_back(0);
    ico.push_back(0);
    writeU16LE(ico, 1);
    writeU16LE(ico, static_cast<std::uint16_t>(bpp));
    writeU32LE(ico, static_cast<std::uint32_t>(dibSize));
    writeU32LE(ico, 22);

    // BITMAPINFOHEADER (40 bytes) — note: double-height per ICO spec
    writeU32LE(ico, 40);
    writeU32LE(ico, static_cast<std::uint32_t>(width));
    writeU32LE(ico, static_cast<std::uint32_t>(height * 2));
    writeU16LE(ico, 1);    // planes
    writeU16LE(ico, static_cast<std::uint16_t>(bpp));
    writeU32LE(ico, 0);    // compression = BI_RGB
    writeU32LE(ico, 0);    // image size (can be 0 for BI_RGB)
    writeU32LE(ico, 0);    // x ppm
    writeU32LE(ico, 0);    // y ppm
    writeU32LE(ico, 0);    // colors used
    writeU32LE(ico, 0);    // important colors

    // Pixel data (BGRA for 32bpp) — fill with opaque blue
    for (int row = 0; row < height; ++row) {
      for (int x = 0; x < width; ++x) {
        if (bpp == 32) {
          ico.push_back(0xFF); // B
          ico.push_back(0x00); // G
          ico.push_back(0x00); // R
          ico.push_back(0xFF); // A
        } else if (bpp == 24) {
          ico.push_back(0xFF); // B
          ico.push_back(0x00); // G
          ico.push_back(0x00); // R
        }
      }
      // Row padding to 4-byte boundary
      int usedBytes = width * (bpp / 8);
      int pad = rowBytes - usedBytes;
      for (int p = 0; p < pad; ++p)
        ico.push_back(0x00);
    }

    // AND mask — all zeros (fully opaque)
    for (int row = 0; row < height; ++row) {
      for (int b = 0; b < andRowBytes; ++b)
        ico.push_back(0x00);
    }

    return ico;
  }

  std::vector<std::uint8_t> buildIcoMultiEntry() {
    // Two entries: 1x1 16bpp, then 1x1 32bpp PNG. The decoder should pick 32bpp.
    std::vector<std::uint8_t> ico;

    // Build a small 1x1 BMP DIB for the 16bpp entry
    const int dibHeaderSize = 40;
    const int pixelRow = 4; // 1 pixel × 2 bytes, padded to 4
    const int andRow = 4;
    const int dibSize = dibHeaderSize + pixelRow + andRow;

    const std::uint32_t pngOffset = 6 + 2 * 16;
    const std::uint32_t bmpOffset = pngOffset + static_cast<std::uint32_t>(sizeof(kPng1x1));

    // ICONDIR
    writeU16LE(ico, 0);
    writeU16LE(ico, 1);
    writeU16LE(ico, 2);

    // Entry 0: 1x1 32bpp PNG (should be selected)
    ico.push_back(1);
    ico.push_back(1);
    ico.push_back(0);
    ico.push_back(0);
    writeU16LE(ico, 1);
    writeU16LE(ico, 32);
    writeU32LE(ico, static_cast<std::uint32_t>(sizeof(kPng1x1)));
    writeU32LE(ico, pngOffset);

    // Entry 1: 1x1 16bpp BMP (should not be selected)
    ico.push_back(1);
    ico.push_back(1);
    ico.push_back(0);
    ico.push_back(0);
    writeU16LE(ico, 1);
    writeU16LE(ico, 16);
    writeU32LE(ico, static_cast<std::uint32_t>(dibSize));
    writeU32LE(ico, bmpOffset);

    // PNG data
    ico.insert(ico.end(), kPng1x1, kPng1x1 + sizeof(kPng1x1));

    // BMP DIB data (dummy)
    std::size_t dibStart = ico.size();
    ico.resize(dibStart + dibSize, 0);
    // Fill BITMAPINFOHEADER
    auto* dib = ico.data() + dibStart;
    dib[0] = 40; // biSize
    dib[4] = 1;  // biWidth = 1
    dib[8] = 2;  // biHeight = 2 (double-height)
    dib[12] = 1; // biPlanes
    dib[14] = 16; // biBitCount

    return ico;
  }

  bool testPngSubImage() {
    auto ico = buildIcoWithPng(1, 1);
    auto result = decodeRasterImage(ico.data(), ico.size());
    if (!checkDecoded(result, "PNG sub-image decode failed"))
      return false;
    bool ok = true;
    ok = check(result->width == 1, "PNG sub-image: width should be 1") && ok;
    ok = check(result->height == 1, "PNG sub-image: height should be 1") && ok;
    ok = check(result->pixels.size() == 4, "PNG sub-image: should have 4 bytes") && ok;
    return ok;
  }

  bool testBmpSubImage() {
    auto ico = buildIcoWithBmp(2, 2, 32);
    auto result = decodeRasterImage(ico.data(), ico.size());
    if (!checkDecoded(result, "BMP sub-image decode failed"))
      return false;
    bool ok = true;
    ok = check(result->width == 2, "BMP sub-image: width should be 2") && ok;
    ok = check(result->height == 2, "BMP sub-image: height should be 2") && ok;
    ok = check(result->pixels.size() == 2 * 2 * 4, "BMP sub-image: should have 16 bytes") && ok;
    // Verify BGRA→RGBA swizzle: input is B=0xFF G=0x00 R=0x00 A=0xFF
    if (result->pixels.size() >= 4) {
      ok = check(result->pixels[0] == 0x00, "BMP sub-image: R should be 0x00") && ok;
      ok = check(result->pixels[1] == 0x00, "BMP sub-image: G should be 0x00") && ok;
      ok = check(result->pixels[2] == 0xFF, "BMP sub-image: B should be 0xFF") && ok;
      ok = check(result->pixels[3] == 0xFF, "BMP sub-image: A should be 0xFF (preserved)") && ok;
    }
    return ok;
  }

  bool testBmpAlphaPreserved() {
    // Build a 2x2 32bpp ICO where pixels have varying alpha
    auto ico = buildIcoWithBmp(2, 2, 32);
    // Patch the first pixel's alpha to 0x80 (semi-transparent).
    // Pixel data starts at offset: 6 (ICONDIR) + 16 (entry) + 40 (DIB header)
    const std::size_t firstPixel = 6 + 16 + 40;
    ico[firstPixel + 3] = 0x80; // A
    // Patch the second pixel to fully transparent.
    ico[firstPixel + 4 + 3] = 0x00; // A

    auto result = decodeRasterImage(ico.data(), ico.size());
    if (!checkDecoded(result, "alpha-preserved decode failed"))
      return false;
    bool ok = true;
    // BMP is bottom-up, so first BMP row = last output row.
    // 2x2 image: BMP row 0 (bottom) → output row 1, BMP row 1 (top) → output row 0.
    const std::size_t bottomRowOut = static_cast<std::size_t>(1) * 2 * 4;
    ok = check(result->pixels[bottomRowOut + 3] == 0x80, "alpha 0x80 should be preserved") && ok;
    ok = check(result->pixels[bottomRowOut + 4 + 3] == 0x00, "alpha 0x00 should be preserved") && ok;
    return ok;
  }

  bool testAndMask() {
    // Build a 2x1 24bpp ICO, then set the AND mask to make the second pixel transparent.
    auto ico = buildIcoWithBmp(2, 1, 24);
    // AND mask starts after pixel data.
    // Pixel data: rowBytes = ((2*24+31)/32)*4 = ((48+31)/32)*4 = (79/32)*4 = 2*4 = 8
    // AND mask: andRowBytes = ((2+31)/32)*4 = 4
    // Offset: 6 (ICONDIR) + 16 (entry) + 40 (DIB header) + 8 (pixel row) = 70
    const std::size_t andMaskOffset = 6 + 16 + 40 + 8;
    // Bit 7 = pixel 0 (opaque), bit 6 = pixel 1 (transparent)
    ico[andMaskOffset] = 0x40;

    auto result = decodeRasterImage(ico.data(), ico.size());
    if (!checkDecoded(result, "AND mask decode failed"))
      return false;
    bool ok = true;
    ok = check(result->width == 2, "AND mask: width should be 2") && ok;
    ok = check(result->height == 1, "AND mask: height should be 1") && ok;
    // Pixel 0 should be opaque (AND bit = 0), pixel 1 should be transparent (AND bit = 1)
    ok = check(result->pixels[3] == 0xFF, "AND mask: pixel 0 alpha should be 0xFF") && ok;
    ok = check(result->pixels[7] == 0x00, "AND mask: pixel 1 alpha should be 0x00") && ok;
    return ok;
  }

  bool testMultiEntryPicksBest() {
    auto ico = buildIcoMultiEntry();
    auto result = decodeRasterImage(ico.data(), ico.size());
    if (!checkDecoded(result, "multi-entry decode failed"))
      return false;
    bool ok = true;
    ok = check(result->width == 1, "multi-entry: should pick 1x1 image") && ok;
    ok = check(result->height == 1, "multi-entry: should pick 1x1 image") && ok;
    return ok;
  }

  bool testEmptyIco() {
    // Valid header but count = 0
    std::vector<std::uint8_t> ico = {0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
    auto result = decodeRasterImage(ico.data(), ico.size());
    bool ok = true;
    ok = check(!result, "empty ICO should fail") && ok;
    if (!result) {
      const bool mentionsNoImages = result.error().find("no images") != std::string::npos;
      ok = check(mentionsNoImages, "empty ICO error should mention 'no images'") && ok;
    }
    return ok;
  }

  bool testTruncatedDirectory() {
    // Claims 1 entry but file is too short to contain it
    std::vector<std::uint8_t> ico = {0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00};
    auto result = decodeRasterImage(ico.data(), ico.size());
    bool ok = true;
    ok = check(!result, "truncated directory should fail") && ok;
    if (!result) {
      const bool mentionsPastEnd = result.error().find("past end") != std::string::npos;
      ok = check(mentionsPastEnd, "truncated error should mention 'past end'") && ok;
    }
    return ok;
  }

  bool testEntryPointsOutsideFile() {
    auto ico = buildIcoWithPng(1, 1);
    // Corrupt the image offset to point past EOF
    ico[18] = 0xFF;
    ico[19] = 0xFF;
    ico[20] = 0x00;
    ico[21] = 0x00;
    auto result = decodeRasterImage(ico.data(), ico.size());
    bool ok = true;
    ok = check(!result, "out-of-bounds entry should fail") && ok;
    if (!result) {
      const bool mentionsOutside = result.error().find("outside") != std::string::npos;
      ok = check(mentionsOutside, "out-of-bounds error should mention 'outside'") && ok;
    }
    return ok;
  }

  bool test256x256Entry() {
    // Width/height 0 in the directory means 256
    auto ico = buildIcoWithPng(256, 256);
    auto result = decodeRasterImage(ico.data(), ico.size());
    if (!checkDecoded(result, "256x256 entry decode failed"))
      return false;
    // The actual decoded size comes from the embedded PNG (1x1), not the directory
    bool ok = true;
    ok = check(result->width == 1, "256x256 entry: decoded width from PNG should be 1") && ok;
    ok = check(result->height == 1, "256x256 entry: decoded height from PNG should be 1") && ok;
    return ok;
  }

} // namespace

int main() {
  bool ok = true;
  ok = testPngSubImage() && ok;
  ok = testBmpSubImage() && ok;
  ok = testBmpAlphaPreserved() && ok;
  ok = testAndMask() && ok;
  ok = testMultiEntryPicksBest() && ok;
  ok = testEmptyIco() && ok;
  ok = testTruncatedDirectory() && ok;
  ok = testEntryPointsOutsideFile() && ok;
  ok = test256x256Entry() && ok;
  return ok ? 0 : 1;
}
