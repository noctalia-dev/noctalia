#pragma once

#include "render/core/texture_manager.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class GlesTextureManager final : public TextureManager {
public:
  GlesTextureManager();
  ~GlesTextureManager() override;

  GlesTextureManager(const GlesTextureManager&) = delete;
  GlesTextureManager& operator=(const GlesTextureManager&) = delete;

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
  void abandonGpuResources() noexcept override;

  void probeExtensions() override;

private:
  TextureHandle uploadPixels(
      const std::uint8_t* data, int width, int height, TextureDataFormat format,
      TextureFilter filter = TextureFilter::Linear, bool mipmap = false
  );
  TextureHandle uploadRgba(const std::uint8_t* data, int width, int height, bool mipmap = false);
  TextureHandle uploadBgra(const std::uint8_t* data, int width, int height, bool mipmap = false);

  // Guards m_textures and m_generation: uploads may run on the decode thread while the main thread unloads.
  mutable std::mutex m_mutex;
  std::vector<TextureId> m_textures;
  std::atomic<std::uint64_t> m_generation{0};
  bool m_hasBgraExt = false;
};
