#pragma once

#include <cstdint>
#include <vector>

// Byte order and alpha meaning of an 8-bit, four-channel resize source. Colour channels are sRGB encoded.
enum class ResizeSourceLayout : std::uint8_t {
  Rgba,              // straight alpha
  RgbaPremultiplied, // premultiplied alpha
  BgraPremultiplied, // premultiplied alpha
  Rgbx,              // fourth byte ignored, result is opaque
  Bgrx,              // fourth byte ignored, result is opaque
};

struct ImageExtent {
  int width = 0;
  int height = 0;
};

// Uniformly scales width x height down so it fits maxWidth x maxHeight; never upscales, and keeps each side >= 1.
[[nodiscard]] ImageExtent fitImageWithin(int width, int height, int maxWidth, int maxHeight) noexcept;

// The single CPU resampler for decoded and captured images: sRGB-aware, alpha-weighted, with stb's default filters
// (Mitchell when shrinking, Catmull-Rom when enlarging). Reads a tightly packed sourceWidth x sourceHeight buffer and
// returns tightly packed straight-alpha RGBA of the target size, or an empty vector when the inputs are invalid.
[[nodiscard]] std::vector<std::uint8_t> resizeToRgba(
    const std::uint8_t* source, int sourceWidth, int sourceHeight, ResizeSourceLayout layout, int targetWidth,
    int targetHeight
);
