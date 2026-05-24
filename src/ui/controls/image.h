#pragma once

#include "render/core/async_texture_cache.h"
#include "render/core/texture_manager.h"
#include "render/scene/node.h"
#include "ui/palette.h"
#include "ui/signal.h"

#include <functional>
#include <string>

class ImageNode;
class Renderer;

enum class ImageFit : std::uint8_t {
  Contain,
  Cover,
  Stretch,
};

class Image : public Node {
public:
  using AsyncReadyCallback = std::function<void()>;

  Image();
  ~Image() override;

  void setRadius(float radius);
  void setBorder(const ColorSpec& color, float width);
  void setBorder(const Color& color, float width);
  void setTint(const Color& tint);
  void setFit(ImageFit fit);
  void setPadding(float padding);
  void setAsyncReadyCallback(AsyncReadyCallback callback);

  bool setSourceFile(Renderer& renderer, const std::string& path, int targetSize = 0, bool mipmap = false);
  bool setSourceFileAsync(
      Renderer& renderer, AsyncTextureCache& cache, const std::string& path, int targetSize = 0, bool mipmap = false
  );
  bool setSourceBytes(Renderer& renderer, const std::uint8_t* data, std::size_t size, bool mipmap = false);
  bool setSourceRaw(
      Renderer& renderer, const std::uint8_t* data, std::size_t size, int width, int height, int stride,
      PixmapFormat format, bool mipmap = false
  );

  // Binds a texture that is owned externally (e.g. by a shared thumbnail
  // cache). The Image will NOT unload the texture on clear or destruction.
  void setExternalTexture(Renderer& renderer, TextureHandle handle);

  void clear(Renderer& renderer);

  [[nodiscard]] const std::string& sourcePath() const noexcept { return m_sourcePath; }
  [[nodiscard]] bool hasImage() const noexcept { return m_texture.valid(); }
  [[nodiscard]] TextureId textureId() const noexcept { return m_texture.id; }
  [[nodiscard]] int sourceWidth() const noexcept { return m_texture.width; }
  [[nodiscard]] int sourceHeight() const noexcept { return m_texture.height; }
  [[nodiscard]] float aspectRatio() const noexcept {
    return m_texture.width > 0 && m_texture.height > 0
               ? static_cast<float>(m_texture.width) / static_cast<float>(m_texture.height)
               : 1.0f;
  }

  void setSize(float width, float height) override;
  void setFrameSize(float width, float height);

private:
  void doLayout(Renderer& renderer) override;
  void applyPalette();
  void updateLayout();
  void clearAsyncSource();
  void subscribeAsyncReady();
  void handleAsyncTextureReady(TextureHandle handle);

  ImageNode* m_image = nullptr;
  TextureHandle m_texture{};
  bool m_ownsTexture = false;
  std::string m_sourcePath;
  int m_sourceRequestedTargetSize = 0;
  int m_sourceTargetSize = 0;
  bool m_sourceMipmap = false;
  float m_radius = 0.0f;
  float m_padding = 0.0f;
  ImageFit m_fit = ImageFit::Contain;
  ColorSpec m_border = clearColorSpec();
  float m_borderWidth = 0.0f;
  Renderer* m_renderer = nullptr;
  AsyncTextureCache* m_asyncTextureCache = nullptr;
  AsyncTextureCache::ReadySubscription m_asyncReadySub;
  std::string m_asyncSourcePath;
  int m_asyncRequestedTargetSize = 0;
  int m_asyncTargetSize = 0;
  bool m_asyncMipmap = false;
  AsyncReadyCallback m_asyncReadyCallback;
  Signal<>::ScopedConnection m_paletteConn;
};
