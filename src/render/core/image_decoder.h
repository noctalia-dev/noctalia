#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

// Upper bound on a WebP canvas we will decode, in RGBA bytes. A WebP VP8X header
// can declare a canvas far larger than its (small) file size, and libwebp would
// allocate the full canvas (twice, for animation) before any downstream check,
// so an attacker-sized canvas is an OOM vector. 256 MiB (~8192x8192 RGBA)
// is far above any real display while bounding the allocation.
inline constexpr std::size_t kMaxWebpCanvasBytes = 256ULL * 1024 * 1024;

struct DecodedRasterImage {
  std::vector<std::uint8_t> pixels;
  int width = 0;
  int height = 0;
};

[[nodiscard]] std::expected<DecodedRasterImage, std::string>
decodeRasterImage(const std::uint8_t* data, std::size_t size);

struct DecodedRasterFrame {
  std::vector<std::uint8_t> rgba; // canvas-size, RGBA8 non-premul
  std::uint32_t durationMs = 0;
};

struct DecodedRasterAnimation {
  int width = 0;
  int height = 0;
  std::vector<DecodedRasterFrame> frames;
  bool truncated = false; // hit cap; later frames omitted
};

// Decode an animated GIF into RGBA frames with composited disposal/blend.
// `maxFrames` and `maxRgbaBytes` cap the resident decoded size; on overrun the
// returned `truncated` flag is set and the partial frames are returned (caller
// may fall back to first-frame-only display).
[[nodiscard]] std::expected<DecodedRasterAnimation, std::string>
decodeAnimatedGif(const std::uint8_t* data, std::size_t size, int maxFrames, std::size_t maxRgbaBytes);
