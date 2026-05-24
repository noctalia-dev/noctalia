#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct DecodedRasterImage {
  std::vector<std::uint8_t> pixels;
  int width = 0;
  int height = 0;
};

[[nodiscard]] std::optional<DecodedRasterImage>
decodeRasterImage(const std::uint8_t* data, std::size_t size, std::string* errorMessage = nullptr);

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
[[nodiscard]] std::optional<DecodedRasterAnimation> decodeAnimatedGif(
    const std::uint8_t* data, std::size_t size, int maxFrames, std::size_t maxRgbaBytes,
    std::string* errorMessage = nullptr
);
