#pragma once

#include "render/core/texture_handle.h"

#include <cstddef>
#include <cstdint>
#include <string>

enum class PixmapFormat {
  RGBA, // Red, Green, Blue, Alpha
  BGRA, // Blue, Green, Red, Alpha
  ARGB, // Alpha, Red, Green, Blue
  RGB,  // Red, Green, Blue (No Alpha)
  BGR   // Blue, Green, Red (No Alpha)
};

enum class TextureDataFormat {
  Alpha,
  LuminanceAlpha,
  Rgba,
};

enum class TextureFilter {
  Nearest,
  Linear,
};

class TextureManager {
public:
  virtual ~TextureManager() = default;

  TextureManager(const TextureManager&) = delete;
  TextureManager& operator=(const TextureManager&) = delete;

  [[nodiscard]] virtual TextureHandle
  loadFromFile(const std::string& path, int targetSize = 0, bool mipmap = false) = 0;
  [[nodiscard]] virtual TextureHandle
  loadFromEncodedBytes(const std::uint8_t* data, std::size_t size, bool mipmap = false) = 0;
  [[nodiscard]] virtual TextureHandle
  loadFromRgba(const std::uint8_t* data, int width, int height, bool mipmap = false) = 0;
  [[nodiscard]] virtual TextureHandle loadFromRaw(
      const std::uint8_t* data, std::size_t size, int width, int height, int stride, PixmapFormat format,
      bool mipmap = false
  ) = 0;
  [[nodiscard]] virtual TextureHandle loadFromPixels(
      const std::uint8_t* data, int width, int height, TextureDataFormat format,
      TextureFilter filter = TextureFilter::Linear, bool mipmap = false
  ) = 0;
  [[nodiscard]] virtual TextureHandle
  createEmpty(int width, int height, TextureDataFormat format, TextureFilter filter = TextureFilter::Linear) = 0;
  virtual bool replace(
      TextureHandle& handle, const std::uint8_t* data, int width, int height, TextureDataFormat format,
      TextureFilter filter = TextureFilter::Linear, bool mipmap = false
  ) = 0;
  virtual bool updateSubImage(
      TextureHandle& handle, const std::uint8_t* data, int x, int y, int width, int height, TextureDataFormat format
  ) = 0;
  virtual void unload(TextureHandle& handle) = 0;
  virtual void cleanup() = 0;

  virtual void probeExtensions() = 0;

protected:
  TextureManager() = default;
};
