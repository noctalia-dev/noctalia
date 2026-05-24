#include "render/core/image_decoder.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>
#include <webp/decode.h>

#define WUFFS_IMPLEMENTATION
#include "wuffs-v0.4.c"

namespace {

  class RgbaDecodeCallbacks final : public wuffs_aux::DecodeImageCallbacks {
  public:
    wuffs_base__pixel_format SelectPixfmt(const wuffs_base__image_config& imageConfig) override {
      (void)imageConfig;
      return wuffs_base__make_pixel_format(WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL);
    }
  };

  // Returns true if the buffer starts with the RIFF....WEBP signature.
  bool isWebP(const std::uint8_t* data, std::size_t size) {
    return size >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' && data[8] == 'W' &&
           data[9] == 'E' && data[10] == 'B' && data[11] == 'P';
  }

  bool isIco(const std::uint8_t* data, std::size_t size) {
    return size >= 6 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01 && data[3] == 0x00;
  }

  bool isPng(const std::uint8_t* data, std::size_t size) {
    return size >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G' && data[4] == 0x0D &&
           data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A;
  }

  std::uint16_t readU16LE(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

  std::uint32_t readU32LE(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
  }

  // ICO files contain a directory of sub-images (PNG or BMP DIB). Pick the
  // largest one and decode it through the normal raster pipeline. For BMP
  // sub-images we prepend a synthetic BITMAPFILEHEADER so wuffs sees a
  // complete BMP.
  std::optional<DecodedRasterImage> decodeIco(const std::uint8_t* data, std::size_t size, std::string* errorMessage) {
    const std::uint16_t count = readU16LE(data + 4);
    if (count == 0) {
      if (errorMessage != nullptr)
        *errorMessage = "ICO file has no images";
      return std::nullopt;
    }

    const std::size_t dirEnd = 6 + static_cast<std::size_t>(count) * 16;
    if (dirEnd > size) {
      if (errorMessage != nullptr)
        *errorMessage = "ICO directory extends past end of file";
      return std::nullopt;
    }

    int bestIdx = -1;
    int bestArea = 0;
    int bestBpp = 0;
    for (int i = 0; i < count; ++i) {
      const std::uint8_t* entry = data + 6 + i * 16;
      int w = entry[0] == 0 ? 256 : entry[0];
      int h = entry[1] == 0 ? 256 : entry[1];
      int bpp = readU16LE(entry + 6);
      int area = w * h;
      if (area > bestArea || (area == bestArea && bpp > bestBpp)) {
        bestArea = area;
        bestBpp = bpp;
        bestIdx = i;
      }
    }

    const std::uint8_t* entry = data + 6 + bestIdx * 16;
    const std::uint32_t imgSize = readU32LE(entry + 8);
    const std::uint32_t imgOffset = readU32LE(entry + 12);

    if (static_cast<std::size_t>(imgOffset) + imgSize > size || imgSize == 0) {
      if (errorMessage != nullptr)
        *errorMessage = "ICO entry points outside file";
      return std::nullopt;
    }

    const std::uint8_t* imgData = data + imgOffset;

    if (isPng(imgData, imgSize)) {
      return decodeRasterImage(imgData, imgSize, errorMessage);
    }

    // BMP DIB sub-image. Standard BMP decoders (wuffs) treat 32bpp as BGRX
    // (alpha forced to 0xFF), but ICO uses that byte as real alpha. Decode
    // 32bpp manually; for other depths fall back to wuffs + AND mask.
    if (imgSize < 40) {
      if (errorMessage != nullptr)
        *errorMessage = "ICO BMP sub-image too small for BITMAPINFOHEADER";
      return std::nullopt;
    }

    const std::uint32_t dibHeaderSize = readU32LE(imgData);
    const std::int32_t dibWidth = static_cast<std::int32_t>(readU32LE(imgData + 4));
    const std::int32_t dibHeight = static_cast<std::int32_t>(readU32LE(imgData + 8));
    const std::uint16_t bpp = readU16LE(imgData + 14);
    const int width = dibWidth > 0 ? dibWidth : -dibWidth;
    const int height = (dibHeight > 0 ? dibHeight : -dibHeight) / 2;

    if (width <= 0 || height <= 0 || width > 1024 || height > 1024) {
      if (errorMessage != nullptr)
        *errorMessage = "ICO BMP sub-image has invalid dimensions";
      return std::nullopt;
    }

    const std::size_t rowStride = static_cast<std::size_t>(((width * bpp + 31) / 32)) * 4;
    const std::size_t pixelDataSize = rowStride * static_cast<std::size_t>(height);
    const std::size_t pixelDataOffset = dibHeaderSize;

    if (pixelDataOffset + pixelDataSize > imgSize) {
      if (errorMessage != nullptr)
        *errorMessage = "ICO BMP pixel data extends past sub-image";
      return std::nullopt;
    }

    if (bpp == 32) {
      DecodedRasterImage decoded;
      decoded.width = width;
      decoded.height = height;
      decoded.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);

      const std::uint8_t* pixels = imgData + pixelDataOffset;
      const bool bottomUp = dibHeight > 0;
      for (int y = 0; y < height; ++y) {
        const int srcRow = bottomUp ? (height - 1 - y) : y;
        const std::uint8_t* srcLine = pixels + static_cast<std::size_t>(srcRow) * rowStride;
        std::uint8_t* dstLine =
            decoded.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        for (int x = 0; x < width; ++x) {
          dstLine[x * 4 + 0] = srcLine[x * 4 + 2]; // R
          dstLine[x * 4 + 1] = srcLine[x * 4 + 1]; // G
          dstLine[x * 4 + 2] = srcLine[x * 4 + 0]; // B
          dstLine[x * 4 + 3] = srcLine[x * 4 + 3]; // A
        }
      }
      return decoded;
    }

    // Non-32bpp: delegate to wuffs, then apply the AND mask for transparency.
    constexpr std::size_t kBmpHeaderSize = 14;
    std::vector<std::uint8_t> bmp(kBmpHeaderSize + imgSize);
    std::memcpy(bmp.data() + kBmpHeaderSize, imgData, imgSize);

    // Fix double-height → real height in the DIB header.
    {
      const std::int32_t realHeight = dibHeight > 0 ? dibHeight / 2 : dibHeight;
      const auto rh = static_cast<std::uint32_t>(realHeight);
      bmp[kBmpHeaderSize + 8] = static_cast<std::uint8_t>(rh & 0xFF);
      bmp[kBmpHeaderSize + 9] = static_cast<std::uint8_t>((rh >> 8) & 0xFF);
      bmp[kBmpHeaderSize + 10] = static_cast<std::uint8_t>((rh >> 16) & 0xFF);
      bmp[kBmpHeaderSize + 11] = static_cast<std::uint8_t>((rh >> 24) & 0xFF);
    }

    const std::uint32_t pixelOffset = static_cast<std::uint32_t>(kBmpHeaderSize + dibHeaderSize);
    const auto totalSize = static_cast<std::uint32_t>(bmp.size());

    bmp[0] = 'B';
    bmp[1] = 'M';
    bmp[2] = static_cast<std::uint8_t>(totalSize & 0xFF);
    bmp[3] = static_cast<std::uint8_t>((totalSize >> 8) & 0xFF);
    bmp[4] = static_cast<std::uint8_t>((totalSize >> 16) & 0xFF);
    bmp[5] = static_cast<std::uint8_t>((totalSize >> 24) & 0xFF);
    bmp[6] = 0;
    bmp[7] = 0;
    bmp[8] = 0;
    bmp[9] = 0;
    bmp[10] = static_cast<std::uint8_t>(pixelOffset & 0xFF);
    bmp[11] = static_cast<std::uint8_t>((pixelOffset >> 8) & 0xFF);
    bmp[12] = static_cast<std::uint8_t>((pixelOffset >> 16) & 0xFF);
    bmp[13] = static_cast<std::uint8_t>((pixelOffset >> 24) & 0xFF);

    auto decoded = decodeRasterImage(bmp.data(), bmp.size(), errorMessage);
    if (!decoded.has_value())
      return std::nullopt;

    // Apply the 1bpp AND mask that follows the pixel data.
    const std::size_t andRowStride = static_cast<std::size_t>(((width + 31) / 32)) * 4;
    const std::size_t andOffset = pixelDataOffset + pixelDataSize;
    if (andOffset + andRowStride * static_cast<std::size_t>(height) <= imgSize) {
      const std::uint8_t* andMask = imgData + andOffset;
      const bool bottomUp = dibHeight > 0;
      for (int y = 0; y < height; ++y) {
        const int maskRow = bottomUp ? (height - 1 - y) : y;
        const std::uint8_t* maskLine = andMask + static_cast<std::size_t>(maskRow) * andRowStride;
        std::uint8_t* dstLine =
            decoded->pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        for (int x = 0; x < width; ++x) {
          if (maskLine[x / 8] & (0x80 >> (x % 8)))
            dstLine[x * 4 + 3] = 0;
        }
      }
    }

    return decoded;
  }

  std::optional<DecodedRasterImage> decodeWebP(const std::uint8_t* data, std::size_t size, std::string* errorMessage) {
    int width = 0, height = 0;
    std::uint8_t* rgba = WebPDecodeRGBA(data, size, &width, &height);
    if (rgba == nullptr) {
      if (errorMessage != nullptr)
        *errorMessage = "libwebp: failed to decode WebP image";
      return std::nullopt;
    }

    DecodedRasterImage decoded;
    decoded.width = width;
    decoded.height = height;
    std::size_t bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    decoded.pixels.resize(bytes);
    std::memcpy(decoded.pixels.data(), rgba, bytes);
    WebPFree(rgba);
    return decoded;
  }

} // namespace

std::optional<DecodedRasterImage>
decodeRasterImage(const std::uint8_t* data, std::size_t size, std::string* errorMessage) {
  if ((data == nullptr) || (size == 0)) {
    if (errorMessage != nullptr) {
      *errorMessage = "empty image buffer";
    }
    return std::nullopt;
  }

  if (isWebP(data, size))
    return decodeWebP(data, size, errorMessage);

  if (isIco(data, size))
    return decodeIco(data, size, errorMessage);

  auto input = wuffs_aux::sync_io::MemoryInput(data, size);
  auto callbacks = RgbaDecodeCallbacks();
  auto result = wuffs_aux::DecodeImage(callbacks, input);
  if (!result.error_message.empty()) {
    if (errorMessage != nullptr) {
      *errorMessage = result.error_message;
    }
    return std::nullopt;
  }

  auto plane = result.pixbuf.plane(0);
  if ((plane.ptr == nullptr) || (plane.width == 0) || (plane.height == 0)) {
    if (errorMessage != nullptr) {
      *errorMessage = "decoded image has no pixel data";
    }
    return std::nullopt;
  }

  DecodedRasterImage decoded;
  decoded.width = static_cast<int>(result.pixbuf.pixcfg.width());
  decoded.height = static_cast<int>(result.pixbuf.pixcfg.height());
  decoded.pixels.resize(plane.width * plane.height);
  std::memcpy(decoded.pixels.data(), plane.ptr, decoded.pixels.size());
  return decoded;
}

namespace {

  bool isGif(const std::uint8_t* data, std::size_t size) {
    return size >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8' &&
           (data[4] == '7' || data[4] == '9') && data[5] == 'a';
  }

  // GIF disposal applied to the persistent canvas BEFORE drawing the next
  // frame, given the previous frame's disposal code and bounds.
  void applyDisposal(
      std::uint8_t* canvas, int canvasWidth, int canvasHeight, wuffs_base__animation_disposal disposal,
      wuffs_base__rect_ie_u32 bounds, const std::uint8_t* previous
  ) {
    if (disposal == WUFFS_BASE__ANIMATION_DISPOSAL__NONE) {
      return;
    }
    const std::uint32_t x0 = std::min<std::uint32_t>(bounds.min_incl_x, static_cast<std::uint32_t>(canvasWidth));
    const std::uint32_t y0 = std::min<std::uint32_t>(bounds.min_incl_y, static_cast<std::uint32_t>(canvasHeight));
    const std::uint32_t x1 = std::min<std::uint32_t>(bounds.max_excl_x, static_cast<std::uint32_t>(canvasWidth));
    const std::uint32_t y1 = std::min<std::uint32_t>(bounds.max_excl_y, static_cast<std::uint32_t>(canvasHeight));
    if (x1 <= x0 || y1 <= y0) {
      return;
    }
    const std::size_t stride = static_cast<std::size_t>(canvasWidth) * 4;
    const std::size_t rowBytes = static_cast<std::size_t>(x1 - x0) * 4;
    if (disposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_BACKGROUND) {
      for (std::uint32_t y = y0; y < y1; ++y) {
        std::memset(canvas + y * stride + x0 * 4, 0, rowBytes);
      }
    } else if (disposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS && previous != nullptr) {
      for (std::uint32_t y = y0; y < y1; ++y) {
        std::memcpy(canvas + y * stride + x0 * 4, previous + y * stride + x0 * 4, rowBytes);
      }
    }
  }

  std::uint32_t clampGifDurationMs(wuffs_base__flicks duration) {
    constexpr std::int64_t kFlicksPerSecond = WUFFS_BASE__FLICKS_PER_SECOND;
    const std::int64_t ms = (static_cast<std::int64_t>(duration) * 1000) / kFlicksPerSecond;
    if (ms < 10) {
      return 100; // browsers' rule for "0 / ASAP" GIFs
    }
    return static_cast<std::uint32_t>(ms);
  }

} // namespace

std::optional<DecodedRasterAnimation> decodeAnimatedGif(
    const std::uint8_t* data, std::size_t size, int maxFrames, std::size_t maxRgbaBytes, std::string* errorMessage
) {
  auto fail = [&](const char* msg) -> std::optional<DecodedRasterAnimation> {
    if (errorMessage != nullptr) {
      *errorMessage = msg;
    }
    return std::nullopt;
  };

  if (data == nullptr || size == 0) {
    return fail("empty image buffer");
  }
  if (!isGif(data, size)) {
    return fail("not a GIF");
  }

  auto dec = wuffs_gif__decoder::alloc_as__wuffs_base__image_decoder();
  if (!dec) {
    return fail("failed to allocate GIF decoder");
  }

  wuffs_base__io_buffer io = wuffs_base__ptr_u8__reader(const_cast<std::uint8_t*>(data), size, /*closed=*/true);

  wuffs_base__image_config imgcfg{};
  {
    wuffs_base__status st = dec->decode_image_config(&imgcfg, &io);
    if (st.repr != nullptr) {
      return fail(st.repr);
    }
  }

  const std::uint32_t width = wuffs_base__pixel_config__width(&imgcfg.pixcfg);
  const std::uint32_t height = wuffs_base__pixel_config__height(&imgcfg.pixcfg);
  if (width == 0 || height == 0) {
    return fail("GIF has zero dimensions");
  }
  const std::uint64_t canvasBytes64 = static_cast<std::uint64_t>(width) * height * 4;
  if (canvasBytes64 > maxRgbaBytes) {
    return fail("GIF canvas exceeds size cap");
  }
  const std::size_t canvasBytes = static_cast<std::size_t>(canvasBytes64);

  wuffs_base__pixel_config pixcfg{};
  wuffs_base__pixel_config__set(
      &pixcfg, WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL, WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, width, height
  );

  std::vector<std::uint8_t> canvas(canvasBytes, 0);
  std::vector<std::uint8_t> previous; // lazy-allocated only if a RESTORE_PREVIOUS frame appears

  wuffs_base__pixel_buffer pb{};
  {
    wuffs_base__status st =
        wuffs_base__pixel_buffer__set_from_slice(&pb, &pixcfg, wuffs_base__make_slice_u8(canvas.data(), canvasBytes));
    if (st.repr != nullptr) {
      return fail(st.repr);
    }
  }

  const std::uint64_t workbufLen = dec->workbuf_len().max_incl;
  std::vector<std::uint8_t> workbuf(static_cast<std::size_t>(workbufLen));
  wuffs_base__slice_u8 workbufSlice = wuffs_base__make_slice_u8(workbuf.data(), workbuf.size());

  DecodedRasterAnimation result;
  result.width = static_cast<int>(width);
  result.height = static_cast<int>(height);

  wuffs_base__animation_disposal prevDisposal = WUFFS_BASE__ANIMATION_DISPOSAL__NONE;
  wuffs_base__rect_ie_u32 prevBounds = wuffs_base__empty_rect_ie_u32();
  std::size_t cumulativeBytes = 0;

  while (true) {
    wuffs_base__frame_config fc{};
    wuffs_base__status fcStatus = dec->decode_frame_config(&fc, &io);
    if (fcStatus.repr == wuffs_base__note__end_of_data) {
      break;
    }
    if (fcStatus.repr != nullptr) {
      if (result.frames.empty()) {
        return fail(fcStatus.repr);
      }
      // Tolerate trailing decode noise after we already have valid frames.
      break;
    }

    applyDisposal(
        canvas.data(), static_cast<int>(width), static_cast<int>(height), prevDisposal, prevBounds,
        previous.empty() ? nullptr : previous.data()
    );

    const wuffs_base__animation_disposal currentDisposal = fc.disposal();
    if (currentDisposal == WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS) {
      if (previous.size() != canvasBytes) {
        previous.assign(canvasBytes, 0);
      }
      std::memcpy(previous.data(), canvas.data(), canvasBytes);
    }

    const wuffs_base__pixel_blend blend =
        fc.overwrite_instead_of_blend() ? WUFFS_BASE__PIXEL_BLEND__SRC : WUFFS_BASE__PIXEL_BLEND__SRC_OVER;

    wuffs_base__status frameStatus = dec->decode_frame(&pb, &io, blend, workbufSlice, nullptr);
    if (frameStatus.repr != nullptr && frameStatus.repr != wuffs_base__note__end_of_data) {
      if (result.frames.empty()) {
        return fail(frameStatus.repr);
      }
      break;
    }

    DecodedRasterFrame frame;
    frame.rgba.assign(canvas.begin(), canvas.end());
    frame.durationMs = clampGifDurationMs(fc.duration());
    cumulativeBytes += canvasBytes;
    result.frames.push_back(std::move(frame));

    prevDisposal = currentDisposal;
    prevBounds = fc.bounds();

    if (static_cast<int>(result.frames.size()) >= maxFrames || cumulativeBytes >= maxRgbaBytes) {
      result.truncated = true;
      break;
    }

    if (frameStatus.repr == wuffs_base__note__end_of_data) {
      break;
    }
  }

  if (result.frames.empty()) {
    return fail("GIF produced no frames");
  }
  return result;
}
