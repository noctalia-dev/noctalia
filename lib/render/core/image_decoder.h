#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

struct DecodedRasterImage {
  std::vector<std::uint8_t> pixels; // RGBA8, straight alpha
  int width = 0;
  int height = 0;
};

struct LoadedImageFile {
  std::vector<std::uint8_t> rgba;
  int width = 0;
  int height = 0;
  // Decoded dimensions before any crop/resize (SVG: rasterized size).
  int sourceWidth = 0;
  int sourceHeight = 0;
};

// Turns encoded images into RGBA pixels. The library ships no codecs in its core: the application
// installs a decoder at startup, either the opt-in codec pack (render/codecs/codecs.h) or its own.
// Both methods are called from worker threads and must be thread safe.
class ImageDecoder {
public:
  virtual ~ImageDecoder() = default;

  // Decodes an encoded image held in memory, applying the container's orientation.
  [[nodiscard]] virtual std::expected<DecodedRasterImage, std::string>
  decode(const std::uint8_t* data, std::size_t size) = 0;

  // Loads an image file or `data:` URI. targetSize > 0 bounds the longer edge; centerSquareCrop crops
  // the result to a centered square.
  [[nodiscard]] virtual std::expected<LoadedImageFile, std::string>
  loadFile(const std::string& path, int targetSize, bool centerSquareCrop) = 0;
};

// Install before any image is loaded; not synchronized with concurrent decodes.
void setImageDecoder(std::unique_ptr<ImageDecoder> decoder);

// Dispatch to the installed decoder. Without one they fail with "no image decoder installed".
std::expected<DecodedRasterImage, std::string> decodeRasterImage(const std::uint8_t* data, std::size_t size);
std::expected<LoadedImageFile, std::string>
loadImageFile(const std::string& path, int targetSize = 0, bool centerSquareCrop = false);
