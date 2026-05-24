#pragma once

#include "render/core/texture_manager.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class GlesTextureManager final : public TextureManager {
public:
  GlesTextureManager() = default;
  ~GlesTextureManager() override;

  GlesTextureManager(const GlesTextureManager&) = delete;
  GlesTextureManager& operator=(const GlesTextureManager&) = delete;

  [[nodiscard]] TextureHandle loadFromFile(const std::string& path, int targetSize = 0, bool mipmap = false) override;
  [[nodiscard]] TextureHandle
  loadFromEncodedBytes(const std::uint8_t* data, std::size_t size, bool mipmap = false) override;
  [[nodiscard]] TextureHandle
  loadFromRgba(const std::uint8_t* data, int width, int height, bool mipmap = false) override;
  [[nodiscard]] TextureHandle loadFromRaw(
      const std::uint8_t* data, std::size_t size, int width, int height, int stride, PixmapFormat format,
      bool mipmap = false
  ) override;
  [[nodiscard]] TextureHandle loadFromPixels(
      const std::uint8_t* data, int width, int height, TextureDataFormat format,
      TextureFilter filter = TextureFilter::Linear, bool mipmap = false
  ) override;
  [[nodiscard]] TextureHandle
  createEmpty(int width, int height, TextureDataFormat format, TextureFilter filter = TextureFilter::Linear) override;
  bool replace(
      TextureHandle& handle, const std::uint8_t* data, int width, int height, TextureDataFormat format,
      TextureFilter filter = TextureFilter::Linear, bool mipmap = false
  ) override;
  bool updateSubImage(
      TextureHandle& handle, const std::uint8_t* data, int x, int y, int width, int height, TextureDataFormat format
  ) override;
  void unload(TextureHandle& handle) override;
  void cleanup() override;

  void probeExtensions() override;

private:
  TextureHandle decodeEncodedRaster(
      const std::uint8_t* data, std::size_t size, const std::string* debugPath = nullptr, bool mipmap = false
  );
  TextureHandle uploadPixels(
      const std::uint8_t* data, int width, int height, TextureDataFormat format,
      TextureFilter filter = TextureFilter::Linear, bool mipmap = false
  );
  TextureHandle uploadRgba(const std::uint8_t* data, int width, int height, bool mipmap = false);
  TextureHandle uploadBgra(const std::uint8_t* data, int width, int height, bool mipmap = false);

  std::vector<TextureId> m_textures;
  bool m_hasBgraExt = false;
};
