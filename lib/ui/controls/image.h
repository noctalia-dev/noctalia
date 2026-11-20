#pragma once

#include "render/core/async_texture_cache.h"
#include "render/core/render_styles.h"
#include "render/scene/node.h"
#include "ui/palette.h"
#include "ui/signal.h"

#include <cstdint>
#include <functional>
#include <memory>
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

enum class ImageLoadState : std::uint8_t {
  Empty,
  Loading,
  Ready,
  Failed,
};

struct ImageSourceOptions {
  // Display size hint in logical px, used until the image is laid out; the laid-out frame then sets the decode size
  // so texels map 1:1 to buffer pixels. 0 decodes at the source size.
  int targetSize = 0;
  // Decode this many times above the frame, for images shown enlarged by a scale transform (dock magnification).
  float oversample = 1.0F;
  bool mipmap = false;
  // Crops the source to its centered square (avatars, album art).
  bool centerSquareCrop = false;
  // Gaussian-like blur radius in logical px.
  float blurRadius = 0.0F;
  // Recolors a symbolic icon: alpha-mask icons keep their alpha, opaque monochrome rasters turn luminance into alpha.
  std::optional<Color> symbolicColor;

  bool operator==(const ImageSourceOptions&) const = default;
};

class Image : public Node {
public:
  using AsyncReadyCallback = std::function<void()>;

  Image();
  ~Image() override;

  void setRadius(float radius);
  // Bind the rounded corner radius to the live corner-radius scale. The stored
  // base radius (times the local surface scale) is reapplied whenever the global
  // scale changes, until an explicit setRadius takes over again.
  void setStyleRadius(float baseRadius, float localScale = 1.0F);
  // Darkens the image's texels with a linear gradient inside the image draw, so a scrimmed image
  // keeps a single antialiased edge instead of needing a rounded rect stacked on top.
  void setScrim(const ImageScrim& scrim);
  void setBorder(const ColorSpec& color, float width);
  void setBorder(const Color& color, float width);
  void setTint(const Color& tint);
  void setSaturation(float saturation);
  void setAppIconColorization(std::optional<ColorSpec> tint);
  // Alpha-mask recolor for bar widget custom_image (widget Color, not app-icon bake).
  void setForegroundTint(std::optional<ColorSpec> tint);
  void setFit(ImageFit fit);
  void setPadding(float padding);
  // Fires when a file source finishes loading (Ready) or fails (Failed); see loadState().
  void setAsyncReadyCallback(AsyncReadyCallback callback);

  // Decodes `path` (a file or data: URI) on the async texture cache workers. Returns true when the texture is already
  // resident and shown; otherwise the image loads in the background and fires the async-ready callback. Changing only
  // the size, scale, or colorization keeps the current texture on screen until the new decode lands.
  bool setSourceFile(Renderer& renderer, const std::string& path, const ImageSourceOptions& options = {});
  // Decodes the current file source again, for a file that changed on disk.
  void reloadSourceFile(Renderer& renderer);
  bool setSourceRaw(
      Renderer& renderer, const std::uint8_t* data, std::size_t size, int width, int height, int stride,
      PixmapFormat format, bool mipmap = false
  );

  // Binds a texture that is owned externally (e.g. by a shared thumbnail
  // cache). The Image will NOT unload the texture on clear or destruction.
  void setExternalTexture(Renderer& renderer, TextureHandle handle);

  void clear(Renderer& renderer);

  ImageLoadState loadState() const noexcept { return m_loadState; }
  const std::string& sourcePath() const noexcept { return m_filePath; }
  bool hasImage() const noexcept { return m_texture.valid(); }
  TextureId textureId() const noexcept { return m_texture.id; }
  int sourceWidth() const noexcept { return m_texture.sourceWidth; }
  int sourceHeight() const noexcept { return m_texture.sourceHeight; }
  float aspectRatio() const noexcept {
    return m_texture.width > 0 && m_texture.height > 0
        ? static_cast<float>(m_texture.width) / static_cast<float>(m_texture.height)
        : 1.0F;
  }

  void setSize(float width, float height) override;
  void setFrameSize(float width, float height);
  // Nothing to show yet while the decode is pending; a retiring texture still on screen counts as content.
  bool hasPendingContent() const noexcept override { return m_loadState == ImageLoadState::Loading && !hasImage(); }

private:
  void doLayout(Renderer& renderer) override;
  void doInvalidateGpuResources(Renderer& renderer) override;
  void doRebindRenderer(Renderer& renderer) override { bindRenderer(renderer); }
  void bindRenderer(Renderer& renderer);
  void applyPalette();
  void updateLayout();
  void applyStyleRadius();
  void setTexture(TextureHandle handle, bool owned);
  void releaseOwnedTexture();

  // File sources (async texture cache).
  [[nodiscard]] ImageDecodeRequest buildDecodeRequest(const Renderer& renderer) const;
  [[nodiscard]] int decodeTargetSize(const Renderer& renderer) const;
  void syncFileRequest();
  void handleDecodeReady(TextureHandle handle);
  void releaseFileRequests();

  // Raw sources keep their pixels so palette changes can rebake the app-icon colorization.
  bool commitRawRgba(Renderer& renderer, const std::uint8_t* rgba, int width, int height, bool mipmap);
  void rebakeRawSource();

  ImageNode* m_image = nullptr;
  TextureHandle m_texture{};
  bool m_ownsTexture = false;
  ImageLoadState m_loadState = ImageLoadState::Empty;
  Renderer* m_renderer = nullptr;

  std::string m_filePath;
  ImageSourceOptions m_fileOptions;
  AsyncTextureCache* m_cache = nullptr;
  // Acquired request whose texture is shown or pending.
  std::optional<ImageDecodeRequest> m_request;
  // Previously shown request kept on screen while m_request decodes.
  std::optional<ImageDecodeRequest> m_retiringRequest;
  AsyncTextureCache::ReadySubscription m_readySub;

  std::vector<std::uint8_t> m_rawRgba;
  int m_rawWidth = 0;
  int m_rawHeight = 0;
  bool m_rawMipmap = false;
  std::optional<Color> m_rawBakedTint;
  bool m_rawBakedLight = false;

  float m_radius = 0.0F;
  std::optional<float> m_styleRadius;
  std::optional<Signal<>::ScopedConnection> m_radiusConn;
  float m_styleRadiusScale = 1.0F;
  float m_padding = 0.0F;
  ImageFit m_fit = ImageFit::Contain;
  ColorSpec m_border = clearColorSpec();
  float m_borderWidth = 0.0F;
  std::optional<ColorSpec> m_appIconColorizeTint;
  std::optional<ColorSpec> m_foregroundTint;
  AsyncReadyCallback m_asyncReadyCallback;
  Signal<>::ScopedConnection m_paletteConn;
};

namespace ui {

  struct ImageProps {
    Image** out = nullptr;
    std::optional<ImageFit> fit = std::nullopt;
    std::optional<float> radius = std::nullopt;
    std::optional<float> styleRadius = std::nullopt;
    std::optional<float> styleRadiusScale = std::nullopt;
    std::optional<float> padding = std::nullopt;
    std::optional<float> width = std::nullopt;
    std::optional<float> height = std::nullopt;
    std::optional<float> flexGrow = std::nullopt;
    std::optional<float> opacity = std::nullopt;
    std::optional<bool> visible = std::nullopt;
    std::optional<bool> participatesInLayout = std::nullopt;
    std::function<void(Image&)> configure = nullptr;
  };

  [[nodiscard]] std::unique_ptr<Image> image(ImageProps props = {});

} // namespace ui
