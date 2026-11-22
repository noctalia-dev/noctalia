#include "ui/controls/image.h"

#include "core/log.h"
#include "render/core/async_texture_cache.h"
#include "render/core/image_decoder.h"
#include "render/core/image_effects.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"
#include "ui/app_icon_colorization.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

  constexpr Logger kLog("image");

  std::vector<std::uint8_t>
  pixmapToRgba(const std::uint8_t* data, std::size_t size, int width, int height, int stride, PixmapFormat format) {
    const std::size_t channels = (format == PixmapFormat::RGB || format == PixmapFormat::BGR) ? 3U : 4U;
    const auto widthSize = static_cast<std::size_t>(width);
    const auto heightSize = static_cast<std::size_t>(height);
    const std::size_t minStride = widthSize * channels;
    const std::size_t actualStride = stride > 0 ? static_cast<std::size_t>(stride) : minStride;
    if (data == nullptr || size == 0 || width <= 0 || height <= 0 || actualStride < minStride) {
      return {};
    }

    const std::size_t requiredSize = (heightSize - 1U) * actualStride + minStride;
    if (size < requiredSize) {
      return {};
    }

    const std::size_t widthBytes4 = widthSize * 4U;
    if (format == PixmapFormat::RGBA && actualStride == widthBytes4) {
      return std::vector<std::uint8_t>(data, data + widthBytes4 * heightSize);
    }

    std::vector<std::uint8_t> rgba(widthSize * heightSize * 4U);
    for (int y = 0; y < height; ++y) {
      const auto row = static_cast<std::size_t>(y);
      const std::uint8_t* srcRow = data + row * actualStride;
      std::uint8_t* dstRow = rgba.data() + row * widthSize * 4U;

      for (int x = 0; x < width; ++x) {
        const std::uint8_t* source = srcRow + static_cast<std::size_t>(x) * channels;
        std::uint8_t* dest = dstRow + static_cast<std::size_t>(x) * 4U;

        switch (format) {
        case PixmapFormat::BGRA:
          dest[0] = source[2];
          dest[1] = source[1];
          dest[2] = source[0];
          dest[3] = source[3];
          break;
        case PixmapFormat::ARGB:
          dest[0] = source[1];
          dest[1] = source[2];
          dest[2] = source[3];
          dest[3] = source[0];
          break;
        case PixmapFormat::RGB:
          dest[0] = source[0];
          dest[1] = source[1];
          dest[2] = source[2];
          dest[3] = 255;
          break;
        case PixmapFormat::BGR:
          dest[0] = source[2];
          dest[1] = source[1];
          dest[2] = source[0];
          dest[3] = 255;
          break;
        case PixmapFormat::RGBA:
          dest[0] = source[0];
          dest[1] = source[1];
          dest[2] = source[2];
          dest[3] = source[3];
          break;
        }
      }
    }

    return rgba;
  }

  ImageFitMode toFitMode(ImageFit fit) {
    switch (fit) {
    case ImageFit::Cover:
      return ImageFitMode::Cover;
    case ImageFit::Contain:
      return ImageFitMode::Contain;
    case ImageFit::Stretch:
      return ImageFitMode::Stretch;
    }
    return ImageFitMode::Stretch;
  }

} // namespace

Image::Image() {
  setClipChildren(true);

  auto image = std::make_unique<ImageNode>();
  m_image = static_cast<ImageNode*>(addChild(std::move(image)));
  m_paletteConn = paletteChanged().connect([this] { applyPalette(); });
}

Image::~Image() {
  releaseFileRequests();
  releaseOwnedTexture();
}

// Retained binding used by teardown and deferred reloads. Callers must hand in
// a renderer that outlives this Image (a RenderTarget's stable view); hosts
// that measure with a transient fixed-scale view must rebind (relayout) against
// the surface's stable renderer before the transient view dies.
void Image::bindRenderer(Renderer& renderer) { m_renderer = &renderer; }

void Image::setRadius(float radius) {
  m_styleRadius.reset();
  m_radiusConn.reset();
  if (m_radius == radius) {
    return;
  }
  m_radius = radius;
  m_image->setRadius(radius);
  markPaintDirty();
}

void Image::setStyleRadius(float baseRadius, float localScale) {
  m_styleRadius = baseRadius;
  m_styleRadiusScale = localScale;
  if (!m_radiusConn) {
    m_radiusConn = Style::cornerRadiusChanged().connect([this] { applyStyleRadius(); });
  }
  applyStyleRadius();
}

void Image::applyStyleRadius() {
  if (!m_styleRadius) {
    return;
  }
  m_radius = Style::scaledRadius(*m_styleRadius, m_styleRadiusScale);
  m_image->setRadius(m_radius);
  markPaintDirty();
}

void Image::setScrim(const ImageScrim& scrim) { m_image->setScrim(scrim); }

void Image::setBorder(const ColorSpec& color, float width) {
  m_border = color;
  m_borderWidth = width;
  applyPalette();
}

void Image::setBorder(const Color& color, float width) { setBorder(fixedColorSpec(color), width); }

void Image::setTint(const Color& tint) {
  m_image->setTint(tint);
  m_image->setMonochromeTint(false);
  m_image->setAlphaMaskTint(false);
  m_foregroundTint = std::nullopt;
  if (m_appIconColorizeTint.has_value()) {
    m_appIconColorizeTint = std::nullopt;
    rebakeRawSource();
    syncFileRequest();
  }
}

void Image::setSaturation(float saturation) { m_image->setSaturation(saturation); }

void Image::setAppIconColorization(std::optional<ColorSpec> tint) {
  if (m_appIconColorizeTint == tint) {
    return;
  }
  m_appIconColorizeTint = tint;
  if (tint.has_value()) {
    m_foregroundTint = std::nullopt;
  }
  applyPalette();
}

void Image::setForegroundTint(std::optional<ColorSpec> tint) {
  if (m_foregroundTint == tint) {
    return;
  }
  m_foregroundTint = tint;
  m_appIconColorizeTint = std::nullopt;
  applyPalette();
}

void Image::setFit(ImageFit fit) {
  if (m_fit == fit) {
    return;
  }
  m_fit = fit;
  updateLayout();
  syncFileRequest();
}

void Image::setPadding(float padding) {
  if (m_padding == padding) {
    return;
  }
  m_padding = padding;
  updateLayout();
}

void Image::setAsyncReadyCallback(AsyncReadyCallback callback) { m_asyncReadyCallback = std::move(callback); }

bool Image::setSourceFile(Renderer& renderer, const std::string& path, const ImageSourceOptions& options) {
  bindRenderer(renderer);
  if (path.empty()) {
    clear(renderer);
    return false;
  }

  AsyncTextureCache* cache = renderer.asyncTextureCache();
  if (cache == nullptr) {
    kLog.warn("no async texture cache bound to the renderer; cannot load {}", path);
    clear(renderer);
    m_loadState = ImageLoadState::Failed;
    return false;
  }

  if (path != m_filePath || cache != m_cache) {
    clear(renderer);
    m_filePath = path;
    m_cache = cache;
  }
  m_fileOptions = options;
  syncFileRequest();
  return m_loadState == ImageLoadState::Ready;
}

void Image::reloadSourceFile(Renderer& renderer) {
  bindRenderer(renderer);
  if (m_filePath.empty() || m_cache == nullptr) {
    return;
  }
  m_cache->invalidatePath(m_filePath);
  syncFileRequest();
}

bool Image::setSourceRaw(
    Renderer& renderer, const std::uint8_t* data, std::size_t size, int width, int height, int stride,
    PixmapFormat format, bool mipmap
) {
  clear(renderer);

  if (data == nullptr || size == 0 || width <= 0 || height <= 0) {
    return false;
  }

  auto rgba = pixmapToRgba(data, size, width, height, stride, format);
  if (rgba.empty() || !commitRawRgba(renderer, rgba.data(), width, height, mipmap)) {
    m_loadState = ImageLoadState::Failed;
    return false;
  }

  m_rawRgba = std::move(rgba);
  m_rawWidth = width;
  m_rawHeight = height;
  m_rawMipmap = mipmap;
  m_loadState = ImageLoadState::Ready;
  return true;
}

void Image::setExternalTexture(Renderer& renderer, TextureHandle handle) {
  if (!m_ownsTexture
      && m_filePath.empty()
      && m_texture.id == handle.id
      && m_texture.width == handle.width
      && m_texture.height == handle.height) {
    return;
  }

  bindRenderer(renderer);
  releaseFileRequests();
  m_filePath.clear();
  m_cache = nullptr;
  m_rawRgba.clear();
  setTexture(handle, false);
  m_loadState = handle.id != 0 ? ImageLoadState::Ready : ImageLoadState::Empty;
}

void Image::clear(Renderer& renderer) {
  bindRenderer(renderer);
  releaseFileRequests();
  releaseOwnedTexture();
  m_filePath.clear();
  m_cache = nullptr;
  m_fileOptions = {};
  m_rawRgba.clear();
  m_rawWidth = 0;
  m_rawHeight = 0;
  m_texture = {};
  m_loadState = ImageLoadState::Empty;
  m_image->setTextureId({});
  m_image->setFrameSize(0.0F, 0.0F);
}

void Image::setTexture(TextureHandle handle, bool owned) {
  if (m_ownsTexture && m_texture.id != 0 && m_texture.id != handle.id && m_renderer != nullptr) {
    m_renderer->textureManager().unload(m_texture);
  }
  m_texture = handle;
  m_ownsTexture = owned && handle.id != 0;
  m_image->setTextureId(m_texture.id);
  updateLayout();
  markPaintDirty();
}

void Image::releaseOwnedTexture() {
  if (m_ownsTexture && m_texture.id != 0 && m_renderer != nullptr) {
    m_renderer->textureManager().unload(m_texture);
    m_texture = {};
  }
  m_ownsTexture = false;
}

ImageDecodeRequest Image::buildDecodeRequest(const Renderer& renderer) const {
  ImageDecodeRequest request{
      .path = m_filePath,
      .targetSize = decodeTargetSize(renderer),
      .mipmap = m_fileOptions.mipmap,
      .centerSquareCrop = m_fileOptions.centerSquareCrop,
      .symbolicColor = m_fileOptions.symbolicColor,
      .generation = m_cache->generation(m_filePath),
  };
  if (m_fileOptions.blurRadius > 0.0F) {
    const float scale = std::max(1.0F, renderer.renderScale());
    request.blurRadius = std::max(1, static_cast<int>(std::round(m_fileOptions.blurRadius * scale)));
  }
  if (m_appIconColorizeTint.has_value()) {
    request.appIconTint = resolveAppIconColorization(*m_appIconColorizeTint).tint;
    request.appIconLightTheme = isResolvedLightTheme();
  }
  return request;
}

int Image::decodeTargetSize(const Renderer& renderer) const {
  if (m_fileOptions.targetSize <= 0) {
    return 0;
  }

  const float scale = renderer.renderScale() > 0.0F ? renderer.renderScale() : 1.0F;
  const float oversample = std::max(1.0F, m_fileOptions.oversample);
  const float paddedWidth = width() - m_padding * 2.0F;
  const float paddedHeight = height() - m_padding * 2.0F;
  if (paddedWidth <= 0.0F || paddedHeight <= 0.0F) {
    // Not laid out yet: the caller's display size stands in for the frame.
    return std::max(1, static_cast<int>(std::round(static_cast<float>(m_fileOptions.targetSize) * scale * oversample)));
  }

  // The frame updateLayout() snaps to whole buffer pixels.
  const float frameWidth = std::round(paddedWidth * scale);
  const float frameHeight = std::round(paddedHeight * scale);
  float side = std::max(frameWidth, frameHeight);
  if (m_fit != ImageFit::Stretch
      && m_texture.valid()
      && !m_ownsTexture
      && m_texture.sourceWidth > 0
      && m_texture.sourceHeight > 0) {
    // Once the source aspect is known, bound the long edge so the fitted texture matches the frame exactly.
    auto sourceWidth = static_cast<float>(m_texture.sourceWidth);
    auto sourceHeight = static_cast<float>(m_texture.sourceHeight);
    if (m_fileOptions.centerSquareCrop) {
      sourceWidth = sourceHeight = std::min(sourceWidth, sourceHeight);
    }
    const float fitScale = m_fit == ImageFit::Cover ? std::max(frameWidth / sourceWidth, frameHeight / sourceHeight)
                                                    : std::min(frameWidth / sourceWidth, frameHeight / sourceHeight);
    side = std::max(sourceWidth, sourceHeight) * fitScale;
  }
  return std::max(1, static_cast<int>(std::round(side * oversample)));
}

void Image::syncFileRequest() {
  if (m_filePath.empty() || m_cache == nullptr || m_renderer == nullptr) {
    return;
  }

  ImageDecodeRequest request = buildDecodeRequest(*m_renderer);
  if (m_request.has_value() && *m_request == request) {
    return;
  }

  m_readySub.disconnect();
  if (m_request.has_value()) {
    // Keep the shown texture on screen until the new decode lands; a pending request was never shown.
    if (m_loadState == ImageLoadState::Ready && !m_retiringRequest.has_value()) {
      m_retiringRequest = std::move(m_request);
    } else {
      m_cache->release(*m_request);
    }
  }

  m_request = std::move(request);
  const TextureHandle handle = m_cache->acquire(*m_request);
  if (handle.id != 0 || m_cache->failed(*m_request)) {
    // Settled synchronously: the caller sees the result directly, so no ready callback.
    const AsyncReadyCallback callback = std::move(m_asyncReadyCallback);
    m_asyncReadyCallback = nullptr;
    handleDecodeReady(handle);
    m_asyncReadyCallback = callback;
    return;
  }

  m_loadState = ImageLoadState::Loading;
  m_readySub = m_cache->subscribeReady(*m_request, [this](TextureHandle ready) { handleDecodeReady(ready); });
}

void Image::handleDecodeReady(TextureHandle handle) {
  m_readySub.disconnect();
  if (handle.id == 0) {
    setTexture({}, false);
    m_loadState = ImageLoadState::Failed;
  } else {
    setTexture(handle, false);
    m_loadState = ImageLoadState::Ready;
  }
  if (m_retiringRequest.has_value()) {
    m_cache->release(*m_retiringRequest);
    m_retiringRequest.reset();
  }
  if (m_asyncReadyCallback) {
    m_asyncReadyCallback();
  }
}

void Image::releaseFileRequests() {
  m_readySub.disconnect();
  if (m_cache != nullptr) {
    if (m_request.has_value()) {
      m_cache->release(*m_request);
    }
    if (m_retiringRequest.has_value()) {
      m_cache->release(*m_retiringRequest);
    }
  }
  m_request.reset();
  m_retiringRequest.reset();
  if (!m_ownsTexture) {
    m_texture = {};
  }
}

bool Image::commitRawRgba(Renderer& renderer, const std::uint8_t* rgba, int width, int height, bool mipmap) {
  bindRenderer(renderer);
  m_rawBakedTint = m_appIconColorizeTint.has_value()
      ? std::optional<Color>(resolveAppIconColorization(*m_appIconColorizeTint).tint)
      : std::nullopt;
  m_rawBakedLight = isResolvedLightTheme();
  TextureHandle texture;
  if (m_rawBakedTint.has_value()) {
    std::vector<std::uint8_t> baked(
        rgba, rgba + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U
    );
    image_effects::colorizeAppIcon(baked.data(), width, height, *m_rawBakedTint, m_rawBakedLight);
    texture = renderer.textureManager().loadFromRgba(baked.data(), width, height, mipmap);
  } else {
    texture = renderer.textureManager().loadFromRgba(rgba, width, height, mipmap);
  }
  if (texture.id == 0) {
    return false;
  }
  setTexture(texture, true);
  return true;
}

void Image::rebakeRawSource() {
  if (m_renderer == nullptr || m_rawRgba.empty()) {
    return;
  }
  commitRawRgba(*m_renderer, m_rawRgba.data(), m_rawWidth, m_rawHeight, m_rawMipmap);
}

void Image::setSize(float width, float height) {
  Node::setSize(width, height);
  updateLayout();
}

void Image::setFrameSize(float width, float height) {
  Node::setFrameSize(width, height);
  updateLayout();
}

void Image::doLayout(Renderer& renderer) {
  bindRenderer(renderer);
  // The render scale can change between layouts (output moves), so resnap the frame first.
  updateLayout();
  syncFileRequest();
}

void Image::doInvalidateGpuResources(Renderer& renderer) {
  bindRenderer(renderer);

  if (m_ownsTexture) {
    if (m_texture.id != 0) {
      renderer.textureManager().unload(m_texture);
    }
    m_texture = {};
    m_ownsTexture = false;
    m_image->setTextureId({});
    rebakeRawSource();
    return;
  }

  if (m_request.has_value() && m_cache != nullptr) {
    // The cache reloads its resident textures and notifies subscribers.
    if (m_retiringRequest.has_value()) {
      m_cache->release(*m_retiringRequest);
      m_retiringRequest.reset();
    }
    m_readySub.disconnect();
    const TextureHandle handle = m_cache->peek(*m_request);
    if (handle.id != 0) {
      setTexture(handle, false);
      m_loadState = ImageLoadState::Ready;
      return;
    }
    setTexture({}, false);
    m_loadState = ImageLoadState::Loading;
    m_readySub = m_cache->subscribeReady(*m_request, [this](TextureHandle ready) { handleDecodeReady(ready); });
    return;
  }

  if (m_texture.id != 0) {
    setTexture({}, false);
  }
}

void Image::applyPalette() {
  m_image->setBorder(resolveColorSpec(m_border), m_borderWidth);

  if (m_appIconColorizeTint.has_value()) {
    m_image->setTint(rgba(1.0F, 1.0F, 1.0F, 1.0F));
    m_image->setMonochromeTint(false);
    m_image->setAlphaMaskTint(false);
  } else if (m_foregroundTint.has_value()) {
    m_image->setTint(resolveColorSpec(*m_foregroundTint));
    m_image->setMonochromeTint(true);
    m_image->setAlphaMaskTint(true);
  } else {
    m_image->setTint(rgba(1.0F, 1.0F, 1.0F, 1.0F));
    m_image->setMonochromeTint(false);
    m_image->setAlphaMaskTint(false);
  }

  // The colorization bake depends on the palette: file sources request the new bake (keeping the old texture until
  // it lands); raw sources rebake on the main thread. Both no-op when the bake inputs did not change.
  syncFileRequest();
  const std::optional<Color> rawTint = m_appIconColorizeTint.has_value()
      ? std::optional<Color>(resolveAppIconColorization(*m_appIconColorizeTint).tint)
      : std::nullopt;
  const bool rawLightTheme = isResolvedLightTheme();
  if (!m_rawRgba.empty() && (rawTint != m_rawBakedTint || (rawTint.has_value() && rawLightTheme != m_rawBakedLight))) {
    rebakeRawSource();
  }
}

void Image::updateLayout() {
  float paddedWidth = std::max(0.0F, width() - m_padding * 2.0F);
  float paddedHeight = std::max(0.0F, height() - m_padding * 2.0F);
  if (m_renderer != nullptr && m_renderer->renderScale() > 0.0F) {
    // Whole buffer pixels, so a texture decoded at this size maps 1:1 once the renderer snaps the origin.
    const float scale = m_renderer->renderScale();
    paddedWidth = std::round(paddedWidth * scale) / scale;
    paddedHeight = std::round(paddedHeight * scale) / scale;
  }
  m_image->setPosition(m_padding, m_padding);
  m_image->setFrameSize(paddedWidth, paddedHeight);
  m_image->setTextureSize(m_texture.width, m_texture.height);
  m_image->setFitMode(toFitMode(m_fit));
}

namespace ui {

  std::unique_ptr<Image> image(ImageProps props) {
    auto control = std::make_unique<Image>();
    if (props.fit.has_value()) {
      control->setFit(*props.fit);
    }
    if (props.styleRadius.has_value()) {
      control->setStyleRadius(*props.styleRadius, props.styleRadiusScale.value_or(1.0F));
    } else if (props.radius.has_value()) {
      control->setRadius(*props.radius);
    }
    if (props.padding.has_value()) {
      control->setPadding(*props.padding);
    }
    if (props.width.has_value() || props.height.has_value()) {
      control->setSize(props.width.value_or(control->width()), props.height.value_or(control->height()));
    }
    if (props.flexGrow.has_value()) {
      control->setFlexGrow(*props.flexGrow);
    }
    if (props.opacity.has_value()) {
      control->setOpacity(*props.opacity);
    }
    if (props.visible.has_value()) {
      control->setVisible(*props.visible);
    }
    if (props.participatesInLayout.has_value()) {
      control->setParticipatesInLayout(*props.participatesInLayout);
    }
    if (props.configure) {
      props.configure(*control);
    }
    if (props.out != nullptr) {
      *props.out = control.get();
    }
    return control;
  }

} // namespace ui
