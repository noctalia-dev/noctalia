#pragma once

#include "render/core/async_texture_cache.h"
#include "render/scene/node.h"
#include "ui/palette.h"
#include "ui/signal.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class ImageNode;
class Renderer;
enum class PixmapFormat;

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
  void setAppIconColorization(std::optional<ColorSpec> tint);
  // Alpha-mask recolor for bar widget custom_image (widget Color, not app-icon bake).
  void setForegroundTint(std::optional<ColorSpec> tint);
  void setFit(ImageFit fit);
  void setPadding(float padding);
  void setAsyncReadyCallback(AsyncReadyCallback callback);

  bool setSourceFile(Renderer& renderer, const std::string& path, int targetSize = 0, bool mipmap = false);
  bool setSourceFile(Renderer& renderer, const std::string& path, int targetSize, bool mipmap, bool centerSquareCrop);
  bool reloadSourceFile(
      Renderer& renderer, const std::string& path, int targetSize = 0, bool mipmap = false,
      bool centerSquareCrop = false
  );
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
  void doInvalidateGpuResources(Renderer& renderer) override;
  void applyPalette();
  void updateLayout();
  void clearAsyncSource();
  void storeOwnedRgbaSource(const std::uint8_t* rgba, int width, int height);
  void clearOwnedRgbaSource();
  void subscribeAsyncReady();
  void handleAsyncTextureReady(TextureHandle handle);
  void presentAsyncTexture(TextureHandle handle);
  void storeColorizationSource(const std::uint8_t* rgba, int width, int height);
  void clearColorizationSource();
  void applyAppIconColorizationPrep(std::uint8_t* rgba, int width, int height);
  bool commitColorizedRgba(Renderer& renderer, const std::uint8_t* rgba, int width, int height, bool mipmap);
  void rebakeColorizedTexture();
  void reloadColorizedSource();
  void reloadUncolorizedSource();

  ImageNode* m_image = nullptr;
  TextureHandle m_texture{};
  bool m_ownsTexture = false;
  std::string m_sourcePath;
  int m_sourceRequestedTargetSize = 0;
  int m_sourceTargetSize = 0;
  bool m_sourceMipmap = false;
  bool m_sourceCenterSquareCrop = false;
  std::vector<std::uint8_t> m_ownedSourceRgba;
  int m_ownedSourceRgbaWidth = 0;
  int m_ownedSourceRgbaHeight = 0;
  float m_radius = 0.0f;
  float m_padding = 0.0f;
  ImageFit m_fit = ImageFit::Contain;
  ColorSpec m_border = clearColorSpec();
  float m_borderWidth = 0.0f;
  std::optional<ColorSpec> m_appIconColorizeTint;
  std::optional<ColorSpec> m_foregroundTint;
  std::vector<std::uint8_t> m_colorizationSource;
  int m_colorizationSourceWidth = 0;
  int m_colorizationSourceHeight = 0;
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
