#include "ui/controls/image.h"

#include "render/core/async_texture_cache.h"
#include "render/core/image_decoder.h"
#include "render/core/image_file_loader.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"
#include "ui/app_icon_colorization.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

  int renderTargetSize(const Renderer& renderer, int targetSize) {
    if (targetSize <= 0) {
      return 0;
    }

    const float scale = std::max(1.0f, renderer.renderScale());
    return std::max(1, static_cast<int>(std::round(static_cast<float>(targetSize) * scale)));
  }

  [[nodiscard]] std::vector<std::uint8_t>
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

} // namespace

Image::Image() {
  setClipChildren(true);

  auto image = std::make_unique<ImageNode>();
  m_image = static_cast<ImageNode*>(addChild(std::move(image)));
  m_paletteConn = paletteChanged().connect([this] { applyPalette(); });
}

Image::~Image() {
  clearAsyncSource();
  if (m_ownsTexture && m_texture.id != 0 && m_renderer != nullptr) {
    m_renderer->textureManager().unload(m_texture);
  }
}

void Image::setRadius(float radius) {
  if (m_radius == radius) {
    return;
  }
  m_radius = radius;
  if (m_image != nullptr) {
    m_image->setRadius(radius);
  }
  markPaintDirty();
}

void Image::setBorder(const ColorSpec& color, float width) {
  m_border = color;
  m_borderWidth = width;
  applyPalette();
}

void Image::setBorder(const Color& color, float width) { setBorder(fixedColorSpec(color), width); }

void Image::setTint(const Color& tint) {
  if (m_image != nullptr) {
    m_image->setTint(tint);
    m_image->setMonochromeTint(false);
    m_image->setAlphaMaskTint(false);
  }
  m_appIconColorizeTint = std::nullopt;
  m_foregroundTint = std::nullopt;
}

void Image::setAppIconColorization(std::optional<ColorSpec> tint) {
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
}

void Image::setPadding(float padding) {
  if (m_padding == padding) {
    return;
  }
  m_padding = padding;
  updateLayout();
}

void Image::setAsyncReadyCallback(AsyncReadyCallback callback) { m_asyncReadyCallback = std::move(callback); }

bool Image::setSourceFile(Renderer& renderer, const std::string& path, int targetSize, bool mipmap) {
  return setSourceFile(renderer, path, targetSize, mipmap, false);
}

bool Image::setSourceFile(
    Renderer& renderer, const std::string& path, int targetSize, bool mipmap, bool centerSquareCrop
) {
  const int requestedTargetSize = std::max(0, targetSize);
  const int textureTargetSize = renderTargetSize(renderer, requestedTargetSize);
  if (m_ownsTexture
      && path == m_sourcePath
      && m_sourceRequestedTargetSize == requestedTargetSize
      && m_sourceTargetSize == textureTargetSize
      && m_sourceMipmap == mipmap
      && m_sourceCenterSquareCrop == centerSquareCrop
      && m_texture.id != 0) {
    return true;
  }

  clear(renderer);
  m_renderer = &renderer;

  if (path.empty()) {
    return false;
  }

  auto loaded = loadImageFile(path, textureTargetSize, centerSquareCrop);
  if (!loaded) {
    m_sourcePath.clear();
    if (m_image != nullptr) {
      m_image->setTextureId({});
    }
    return false;
  }

  if (!commitColorizedRgba(renderer, loaded->rgba.data(), loaded->width, loaded->height, mipmap)) {
    m_sourcePath.clear();
    if (m_image != nullptr) {
      m_image->setTextureId({});
    }
    return false;
  }

  m_sourcePath = path;
  m_sourceRequestedTargetSize = requestedTargetSize;
  m_sourceTargetSize = textureTargetSize;
  m_sourceMipmap = mipmap;
  m_sourceCenterSquareCrop = centerSquareCrop;
  clearOwnedRgbaSource();
  updateLayout();
  return true;
}

bool Image::reloadSourceFile(
    Renderer& renderer, const std::string& path, int targetSize, bool mipmap, bool centerSquareCrop
) {
  m_renderer = &renderer;

  if (path.empty()) {
    return false;
  }

  const int requestedTargetSize = std::max(0, targetSize);
  const int textureTargetSize = renderTargetSize(renderer, requestedTargetSize);
  auto loaded = loadImageFile(path, textureTargetSize, centerSquareCrop);
  if (!loaded) {
    return false;
  }

  clearAsyncSource();
  if (!commitColorizedRgba(renderer, loaded->rgba.data(), loaded->width, loaded->height, mipmap)) {
    return false;
  }

  m_sourcePath = path;
  m_sourceRequestedTargetSize = requestedTargetSize;
  m_sourceTargetSize = textureTargetSize;
  m_sourceMipmap = mipmap;
  m_sourceCenterSquareCrop = centerSquareCrop;
  clearOwnedRgbaSource();
  if (m_image != nullptr) {
    m_image->setTextureId(m_texture.id);
  }
  updateLayout();
  return true;
}

bool Image::setSourceFileAsync(
    Renderer& renderer, AsyncTextureCache& cache, const std::string& path, int targetSize, bool mipmap
) {
  m_renderer = &renderer;

  const int requestedTargetSize = std::max(0, targetSize);
  const int normalizedTargetSize = renderTargetSize(renderer, requestedTargetSize);
  if (path.empty()) {
    clear(renderer);
    return false;
  }

  const bool sameRequest = m_asyncTextureCache == &cache
      && m_asyncSourcePath == path
      && m_asyncRequestedTargetSize == requestedTargetSize
      && m_asyncTargetSize == normalizedTargetSize
      && m_asyncMipmap == mipmap;
  if (sameRequest && m_texture.id != 0) {
    if (m_appIconColorizeTint.has_value() && !m_ownsTexture) {
      presentAsyncTexture(m_texture);
    }
    return hasImage();
  }

  if (!sameRequest) {
    clear(renderer);
    m_asyncTextureCache = &cache;
    m_asyncSourcePath = path;
    m_asyncRequestedTargetSize = requestedTargetSize;
    m_asyncTargetSize = normalizedTargetSize;
    m_asyncMipmap = mipmap;
    m_texture = cache.acquire(path, normalizedTargetSize, mipmap);
    if (m_texture.id == 0) {
      subscribeAsyncReady();
    }
  } else if (m_texture.id == 0) {
    m_texture = cache.peek(path, normalizedTargetSize, mipmap);
    if (m_texture.id != 0) {
      m_asyncReadySub.disconnect();
    }
  }

  if (m_texture.id == 0) {
    m_sourcePath = path;
    if (m_image != nullptr) {
      m_image->setTextureId({});
      m_image->setFrameSize(0.0f, 0.0f);
    }
    return false;
  }

  presentAsyncTexture(m_texture);
  return hasImage();
}

bool Image::setSourceBytes(Renderer& renderer, const std::uint8_t* data, std::size_t size, bool mipmap) {
  clear(renderer);
  m_renderer = &renderer;

  if (data == nullptr || size == 0) {
    return false;
  }

  auto decoded = decodeRasterImage(data, size);
  if (!decoded) {
    m_sourcePath.clear();
    if (m_image != nullptr) {
      m_image->setTextureId({});
    }
    return false;
  }

  if (!commitColorizedRgba(renderer, decoded->pixels.data(), decoded->width, decoded->height, mipmap)) {
    m_sourcePath.clear();
    if (m_image != nullptr) {
      m_image->setTextureId({});
    }
    return false;
  }

  m_sourcePath.clear();
  m_sourceMipmap = mipmap;
  storeOwnedRgbaSource(decoded->pixels.data(), decoded->width, decoded->height);
  updateLayout();
  return true;
}

bool Image::setSourceRaw(
    Renderer& renderer, const std::uint8_t* data, std::size_t size, int width, int height, int stride,
    PixmapFormat format, bool mipmap
) {
  clear(renderer);
  m_renderer = &renderer;

  if (data == nullptr || size == 0 || width <= 0 || height <= 0) {
    return false;
  }

  auto rgba = pixmapToRgba(data, size, width, height, stride, format);
  if (rgba.empty()) {
    m_sourcePath.clear();
    if (m_image != nullptr) {
      m_image->setTextureId({});
    }
    return false;
  }

  if (!commitColorizedRgba(renderer, rgba.data(), width, height, mipmap)) {
    m_sourcePath.clear();
    if (m_image != nullptr) {
      m_image->setTextureId({});
    }
    return false;
  }

  m_sourcePath.clear();
  m_sourceMipmap = mipmap;
  storeOwnedRgbaSource(rgba.data(), width, height);
  updateLayout();
  return true;
}

void Image::setExternalTexture(Renderer& renderer, TextureHandle handle) {
  if (!m_ownsTexture
      && m_texture.id == handle.id
      && m_texture.width == handle.width
      && m_texture.height == handle.height) {
    return;
  }

  m_renderer = &renderer;
  clearAsyncSource();
  clearColorizationSource();
  if (m_ownsTexture && m_texture.id != 0) {
    renderer.textureManager().unload(m_texture);
  }

  m_texture = handle;
  m_ownsTexture = false;
  m_sourcePath.clear();
  m_sourceRequestedTargetSize = 0;
  m_sourceTargetSize = 0;
  m_sourceMipmap = false;
  clearOwnedRgbaSource();
  if (m_image != nullptr) {
    m_image->setTextureId(m_texture.id);
  }
  updateLayout();
}

void Image::clear(Renderer& renderer) {
  m_renderer = &renderer;
  clearAsyncSource();
  if (m_ownsTexture && m_texture.id != 0) {
    renderer.textureManager().unload(m_texture);
  }
  m_texture = {};
  m_ownsTexture = false;
  m_sourcePath.clear();
  m_sourceRequestedTargetSize = 0;
  m_sourceTargetSize = 0;
  m_sourceMipmap = false;
  m_sourceCenterSquareCrop = false;
  clearOwnedRgbaSource();
  clearColorizationSource();
  if (m_image != nullptr) {
    m_image->setTextureId({});
    m_image->setFrameSize(0.0f, 0.0f);
  }
}

void Image::clearAsyncSource() {
  m_asyncReadySub.disconnect();
  if (m_asyncTextureCache != nullptr && !m_asyncSourcePath.empty()) {
    m_asyncTextureCache->release(m_asyncSourcePath, m_asyncTargetSize, m_asyncMipmap);
  }
  m_asyncTextureCache = nullptr;
  m_asyncSourcePath.clear();
  m_asyncRequestedTargetSize = 0;
  m_asyncTargetSize = 0;
  m_asyncMipmap = false;
}

void Image::storeOwnedRgbaSource(const std::uint8_t* rgba, int width, int height) {
  if (rgba == nullptr || width <= 0 || height <= 0) {
    clearOwnedRgbaSource();
    return;
  }
  const auto byteCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
  m_ownedSourceRgba.assign(rgba, rgba + byteCount);
  m_ownedSourceRgbaWidth = width;
  m_ownedSourceRgbaHeight = height;
}

void Image::clearOwnedRgbaSource() {
  m_ownedSourceRgba.clear();
  m_ownedSourceRgbaWidth = 0;
  m_ownedSourceRgbaHeight = 0;
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
  if (m_ownsTexture && !m_sourcePath.empty() && m_sourceRequestedTargetSize > 0) {
    const int textureTargetSize = renderTargetSize(renderer, m_sourceRequestedTargetSize);
    if (textureTargetSize != m_sourceTargetSize) {
      auto loaded = loadImageFile(m_sourcePath, textureTargetSize, m_sourceCenterSquareCrop);
      if (loaded) {
        if (commitColorizedRgba(renderer, loaded->rgba.data(), loaded->width, loaded->height, m_sourceMipmap)) {
          m_sourceTargetSize = textureTargetSize;
          updateLayout();
        }
      }
    }
  }

  if (m_asyncTextureCache != nullptr && !m_asyncSourcePath.empty()) {
    const int textureTargetSize = renderTargetSize(renderer, m_asyncRequestedTargetSize);
    if (textureTargetSize != m_asyncTargetSize) {
      m_asyncReadySub.disconnect();
      m_asyncTextureCache->release(m_asyncSourcePath, m_asyncTargetSize, m_asyncMipmap);
      m_texture = {};
      m_ownsTexture = false;
      m_asyncTargetSize = textureTargetSize;
      if (m_image != nullptr) {
        m_image->setTextureId({});
        m_image->setFrameSize(0.0f, 0.0f);
      }
      m_texture = m_asyncTextureCache->acquire(m_asyncSourcePath, m_asyncTargetSize, m_asyncMipmap);
      if (m_texture.id != 0) {
        presentAsyncTexture(m_texture);
      } else {
        subscribeAsyncReady();
      }
    }
  }

  if (m_texture.id == 0 && m_asyncTextureCache != nullptr && !m_asyncSourcePath.empty()) {
    const auto handle = m_asyncTextureCache->peek(m_asyncSourcePath, m_asyncTargetSize, m_asyncMipmap);
    if (handle.id != 0) {
      presentAsyncTexture(handle);
    }
  }
}

void Image::doInvalidateGpuResources(Renderer& renderer) {
  m_renderer = &renderer;

  if (m_ownsTexture) {
    if (m_texture.id != 0) {
      renderer.textureManager().unload(m_texture);
    }
    m_texture = {};

    bool reloaded = false;
    if (m_appIconColorizeTint.has_value() && !m_colorizationSource.empty()) {
      rebakeColorizedTexture();
      reloaded = m_texture.id != 0;
    } else if (!m_sourcePath.empty()) {
      reloaded = reloadSourceFile(
          renderer, m_sourcePath, m_sourceRequestedTargetSize, m_sourceMipmap, m_sourceCenterSquareCrop
      );
    } else if (!m_ownedSourceRgba.empty() && m_ownedSourceRgbaWidth > 0 && m_ownedSourceRgbaHeight > 0) {
      reloaded = commitColorizedRgba(
          renderer, m_ownedSourceRgba.data(), m_ownedSourceRgbaWidth, m_ownedSourceRgbaHeight, m_sourceMipmap
      );
    }

    if (!reloaded && m_image != nullptr) {
      m_image->setTextureId({});
      m_image->setTextureSize(0, 0);
    }
    updateLayout();
    markPaintDirty();
    return;
  }

  if (m_asyncTextureCache != nullptr && !m_asyncSourcePath.empty()) {
    const TextureHandle handle = m_asyncTextureCache->peek(m_asyncSourcePath, m_asyncTargetSize, m_asyncMipmap);
    if (handle.id != 0) {
      m_texture = handle;
      if (m_image != nullptr) {
        m_image->setTextureId(m_texture.id);
      }
      updateLayout();
      markPaintDirty();
      return;
    }

    m_texture = {};
    if (m_image != nullptr) {
      m_image->setTextureId({});
      m_image->setTextureSize(0, 0);
    }
    subscribeAsyncReady();
    updateLayout();
    markPaintDirty();
    return;
  }

  if (m_texture.id != 0) {
    m_texture = {};
    if (m_image != nullptr) {
      m_image->setTextureId({});
      m_image->setTextureSize(0, 0);
    }
    updateLayout();
    markPaintDirty();
  }
}

void Image::subscribeAsyncReady() {
  m_asyncReadySub.disconnect();
  if (m_asyncTextureCache == nullptr || m_asyncSourcePath.empty()) {
    return;
  }

  m_asyncReadySub = m_asyncTextureCache->subscribeReady(
      m_asyncSourcePath, m_asyncTargetSize, m_asyncMipmap,
      [this](TextureHandle handle) { handleAsyncTextureReady(handle); }
  );
}

void Image::handleAsyncTextureReady(TextureHandle handle) {
  if (handle.id == 0 || m_asyncTextureCache == nullptr || m_asyncSourcePath.empty() || m_renderer == nullptr) {
    return;
  }
  presentAsyncTexture(handle);
}

void Image::presentAsyncTexture(TextureHandle handle) {
  if (handle.id == 0 || m_renderer == nullptr) {
    return;
  }

  if (m_appIconColorizeTint.has_value() && m_asyncTextureCache != nullptr && !m_asyncSourcePath.empty()) {
    if (auto loaded = loadImageFile(m_asyncSourcePath, m_asyncTargetSize)) {
      if (commitColorizedRgba(*m_renderer, loaded->rgba.data(), loaded->width, loaded->height, m_asyncMipmap)) {
        m_asyncTextureCache->release(m_asyncSourcePath, m_asyncTargetSize, m_asyncMipmap);
        m_sourcePath = m_asyncSourcePath;
      }
    }
  } else {
    m_texture = handle;
    m_ownsTexture = false;
    m_sourcePath = m_asyncSourcePath;
    if (m_image != nullptr) {
      m_image->setTextureId(m_texture.id);
    }
  }

  m_asyncReadySub.disconnect();
  updateLayout();
  markPaintDirty();

  if (m_asyncReadyCallback) {
    m_asyncReadyCallback();
  }
}

void Image::storeColorizationSource(const std::uint8_t* rgba, int width, int height) {
  if (!m_appIconColorizeTint.has_value() || rgba == nullptr || width <= 0 || height <= 0) {
    clearColorizationSource();
    return;
  }

  const auto byteCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
  m_colorizationSource.assign(rgba, rgba + byteCount);
  m_colorizationSourceWidth = width;
  m_colorizationSourceHeight = height;
}

void Image::clearColorizationSource() {
  m_colorizationSource.clear();
  m_colorizationSourceWidth = 0;
  m_colorizationSourceHeight = 0;
}

void Image::applyAppIconColorizationPrep(std::uint8_t* rgba, int width, int height) {
  if (!m_appIconColorizeTint.has_value()) {
    return;
  }
  const AppIconColorizationStyle style = resolveAppIconColorization(*m_appIconColorizeTint);
  bakeAppIconForColorization(rgba, width, height, style.tint);
}

bool Image::commitColorizedRgba(Renderer& renderer, const std::uint8_t* rgba, int width, int height, bool mipmap) {
  if (rgba == nullptr || width <= 0 || height <= 0) {
    return false;
  }

  if (!m_appIconColorizeTint.has_value()) {
    clearColorizationSource();
    auto texture = renderer.textureManager().loadFromRgba(rgba, width, height, mipmap);
    if (texture.id == 0) {
      return false;
    }
    if (m_ownsTexture && m_texture.id != 0) {
      renderer.textureManager().unload(m_texture);
    }
    m_texture = texture;
    m_ownsTexture = true;
    if (m_image != nullptr) {
      m_image->setTextureId(m_texture.id);
    }
    return true;
  }

  storeColorizationSource(rgba, width, height);
  std::vector<std::uint8_t> baked = m_colorizationSource;
  applyAppIconColorizationPrep(baked.data(), width, height);

  auto texture = renderer.textureManager().loadFromRgba(baked.data(), width, height, mipmap);
  if (texture.id == 0) {
    return false;
  }

  if (m_ownsTexture && m_texture.id != 0) {
    renderer.textureManager().unload(m_texture);
  }
  m_texture = texture;
  m_ownsTexture = true;
  if (m_image != nullptr) {
    m_image->setTextureId(m_texture.id);
  }
  return true;
}

void Image::rebakeColorizedTexture() {
  if (m_renderer == nullptr
      || !m_appIconColorizeTint.has_value()
      || m_colorizationSource.empty()
      || m_colorizationSourceWidth <= 0
      || m_colorizationSourceHeight <= 0) {
    return;
  }

  std::vector<std::uint8_t> baked = m_colorizationSource;
  applyAppIconColorizationPrep(baked.data(), m_colorizationSourceWidth, m_colorizationSourceHeight);

  auto texture = m_renderer->textureManager().loadFromRgba(
      baked.data(), m_colorizationSourceWidth, m_colorizationSourceHeight, m_sourceMipmap
  );
  if (texture.id == 0) {
    return;
  }

  if (m_ownsTexture && m_texture.id != 0) {
    m_renderer->textureManager().unload(m_texture);
  }
  m_texture = texture;
  m_ownsTexture = true;
  if (m_image != nullptr) {
    m_image->setTextureId(m_texture.id);
  }
  updateLayout();
  markPaintDirty();
}

void Image::reloadColorizedSource() {
  if (m_renderer == nullptr || !m_appIconColorizeTint.has_value()) {
    return;
  }
  if (!m_colorizationSource.empty()) {
    rebakeColorizedTexture();
    return;
  }
  if (!m_sourcePath.empty() && m_texture.id != 0) {
    (void)reloadSourceFile(
        *m_renderer, m_sourcePath, m_sourceRequestedTargetSize, m_sourceMipmap, m_sourceCenterSquareCrop
    );
  }
}

void Image::reloadUncolorizedSource() {
  if (m_renderer == nullptr || m_appIconColorizeTint.has_value()) {
    return;
  }

  if (!m_colorizationSource.empty() && m_colorizationSourceWidth > 0 && m_colorizationSourceHeight > 0) {
    (void)commitColorizedRgba(
        *m_renderer, m_colorizationSource.data(), m_colorizationSourceWidth, m_colorizationSourceHeight, m_sourceMipmap
    );
    updateLayout();
    markPaintDirty();
    return;
  }

  if (!m_sourcePath.empty()) {
    (void)reloadSourceFile(
        *m_renderer, m_sourcePath, m_sourceRequestedTargetSize, m_sourceMipmap, m_sourceCenterSquareCrop
    );
    return;
  }

  if (!m_ownedSourceRgba.empty() && m_ownedSourceRgbaWidth > 0 && m_ownedSourceRgbaHeight > 0) {
    (void)commitColorizedRgba(
        *m_renderer, m_ownedSourceRgba.data(), m_ownedSourceRgbaWidth, m_ownedSourceRgbaHeight, m_sourceMipmap
    );
    updateLayout();
    markPaintDirty();
    return;
  }

  if (m_asyncTextureCache != nullptr && !m_asyncSourcePath.empty()) {
    const TextureHandle handle = m_asyncTextureCache->peek(m_asyncSourcePath, m_asyncTargetSize, m_asyncMipmap);
    if (handle.id != 0) {
      presentAsyncTexture(handle);
    }
  }
}

void Image::applyPalette() {
  const Color border = resolveColorSpec(m_border);
  if (m_image == nullptr) {
    return;
  }

  m_image->setBorder(border, m_borderWidth);
  if (!m_appIconColorizeTint.has_value() && !m_colorizationSource.empty()) {
    reloadUncolorizedSource();
  }
  if (m_appIconColorizeTint.has_value()) {
    m_image->setTint(rgba(1.0f, 1.0f, 1.0f, 1.0f));
    m_image->setMonochromeTint(false);
    m_image->setAlphaMaskTint(false);
    reloadColorizedSource();
    return;
  }

  if (m_foregroundTint.has_value()) {
    m_image->setTint(resolveColorSpec(*m_foregroundTint));
    m_image->setMonochromeTint(true);
    m_image->setAlphaMaskTint(true);
    return;
  }

  m_image->setTint(rgba(1.0f, 1.0f, 1.0f, 1.0f));
  m_image->setMonochromeTint(false);
  m_image->setAlphaMaskTint(false);
}

void Image::updateLayout() {
  if (m_image == nullptr) {
    return;
  }

  const float paddedWidth = std::max(0.0f, width() - m_padding * 2.0f);
  const float paddedHeight = std::max(0.0f, height() - m_padding * 2.0f);
  m_image->setPosition(m_padding, m_padding);
  m_image->setFrameSize(paddedWidth, paddedHeight);
  m_image->setTextureSize(m_texture.width, m_texture.height);

  ImageFitMode mode = ImageFitMode::Stretch;
  switch (m_fit) {
  case ImageFit::Cover:
    mode = ImageFitMode::Cover;
    break;
  case ImageFit::Contain:
    mode = ImageFitMode::Contain;
    break;
  case ImageFit::Stretch:
    mode = ImageFitMode::Stretch;
    break;
  }
  m_image->setFitMode(mode);
}
