#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Encode an RGBA8 (non-premultiplied) pixel buffer to image file bytes. Counterpart to image_decoder.
// Callers that need a file write the returned buffer themselves (avoids re-encoding).
// Every encoder returns an empty buffer on failure and, when `errorOut` is set, the reason.

[[nodiscard]] std::vector<std::uint8_t>
encodePng(const std::uint8_t* rgba, int width, int height, std::string* errorOut = nullptr);

// Baseline JPEG; alpha is discarded. `quality` is 1..100.
[[nodiscard]] std::vector<std::uint8_t>
encodeJpeg(const std::uint8_t* rgba, int width, int height, int quality, std::string* errorOut = nullptr);

// Lossy WebP for `quality` 1..99, lossless WebP at 100.
[[nodiscard]] std::vector<std::uint8_t>
encodeWebp(const std::uint8_t* rgba, int width, int height, int quality, std::string* errorOut = nullptr);

// Lossy JPEG XL for `quality` 1..99, lossless JPEG XL at 100.
[[nodiscard]] std::vector<std::uint8_t>
encodeJxl(const std::uint8_t* rgba, int width, int height, int quality, std::string* errorOut = nullptr);
