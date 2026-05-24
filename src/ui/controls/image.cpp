#include "ui/controls/image.h"

#include "render/core/async_texture_cache.h"
#include "render/core/renderer.h"
#include "render/scene/image_node.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

  int renderTargetSize(const Renderer& renderer, int targetSize) {
    if (targetSize <= 0) {
      return 0;
    }

    const float scale = std::max(1.0f, renderer.renderScale());
    return std::max(1, static_cast<int>(std::round(static_cast<float>(targetSize) * scale)));
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
  }
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
  const int requestedTargetSize = std::max(0, targetSize);
  const int textureTargetSize = renderTargetSize(renderer, requestedTargetSize);
  if (m_ownsTexture && path == m_sourcePath && m_sourceRequestedTargetSize == requestedTargetSize &&
      m_sourceTargetSize == textureTargetSize && m_sourceMipmap == mipmap && m_texture.id != 0) {
    return true;
  }

  clear(renderer);
  m_renderer = &renderer;

  if (path.empty()) {
    return false;
  }

  m_texture = renderer.textureManager().loadFromFile(path, textureTargetSize, mipmap);
  if (m_texture.id == 0) {
    m_sourcePath.clear();
    if (m_image != nullptr) {
      m_image->setTextureId({});
    }
    return false;
  }

  m_ownsTexture = true;
  m_sourcePath = path;
  m_sourceRequestedTargetSize = requestedTargetSize;
  m_sourceTargetSize = textureTargetSize;
  m_sourceMipmap = mipmap;
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

  const bool sameRequest = m_asyncTextureCache == &cache && m_asyncSourcePath == path &&
                           m_asyncRequestedTargetSize == requestedTargetSize &&
                           m_asyncTargetSize == normalizedTargetSize && m_asyncMipmap == mipmap;
  if (sameRequest && m_texture.id != 0) {
    return true;
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

  m_ownsTexture = false;
  m_sourcePath = path;
  if (m_image != nullptr) {
    m_image->setTextureId(m_texture.id);
  }
  updateLayout();
  return true;
}

bool Image::setSourceBytes(Renderer& renderer, const std::uint8_t* data, std::size_t size, bool mipmap) {
  clear(renderer);
  m_renderer = &renderer;

  if (data == nullptr || size == 0) {
    return false;
  }

  m_texture = renderer.textureManager().loadFromEncodedBytes(data, size, mipmap);
  if (m_texture.id == 0) {
    m_sourcePath.clear();
    if (m_image != nullptr) {
      m_image->setTextureId({});
    }
    return false;
  }

  m_ownsTexture = true;
  m_sourcePath.clear();
  if (m_image != nullptr) {
    m_image->setTextureId(m_texture.id);
  }
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

  m_texture = renderer.textureManager().loadFromRaw(data, size, width, height, stride, format, mipmap);
  if (m_texture.id == 0) {
    m_sourcePath.clear();
    if (m_image != nullptr) {
      m_image->setTextureId({});
    }
    return false;
  }

  m_ownsTexture = true;
  m_sourcePath.clear();
  if (m_image != nullptr) {
    m_image->setTextureId(m_texture.id);
  }
  updateLayout();
  return true;
}

void Image::setExternalTexture(Renderer& renderer, TextureHandle handle) {
  if (!m_ownsTexture && m_texture.id == handle.id && m_texture.width == handle.width &&
      m_texture.height == handle.height) {
    return;
  }

  m_renderer = &renderer;
  clearAsyncSource();
  if (m_ownsTexture && m_texture.id != 0) {
    renderer.textureManager().unload(m_texture);
  }

  m_texture = handle;
  m_ownsTexture = false;
  m_sourcePath.clear();
  m_sourceRequestedTargetSize = 0;
  m_sourceTargetSize = 0;
  m_sourceMipmap = false;
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
      auto texture = renderer.textureManager().loadFromFile(m_sourcePath, textureTargetSize, m_sourceMipmap);
      if (texture.id != 0) {
        if (m_texture.id != 0) {
          renderer.textureManager().unload(m_texture);
        }
        m_texture = texture;
        m_sourceTargetSize = textureTargetSize;
        if (m_image != nullptr) {
          m_image->setTextureId(m_texture.id);
        }
        updateLayout();
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
        m_sourcePath = m_asyncSourcePath;
        if (m_image != nullptr) {
          m_image->setTextureId(m_texture.id);
        }
        updateLayout();
      } else {
        subscribeAsyncReady();
      }
    }
  }

  if (m_texture.id == 0 && m_asyncTextureCache != nullptr && !m_asyncSourcePath.empty()) {
    const auto handle = m_asyncTextureCache->peek(m_asyncSourcePath, m_asyncTargetSize, m_asyncMipmap);
    if (handle.id != 0) {
      m_texture = handle;
      m_ownsTexture = false;
      m_sourcePath = m_asyncSourcePath;
      if (m_image != nullptr) {
        m_image->setTextureId(m_texture.id);
      }
      m_asyncReadySub.disconnect();
      updateLayout();
      if (m_asyncReadyCallback) {
        m_asyncReadyCallback();
      }
    }
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
  if (handle.id == 0 || m_asyncTextureCache == nullptr || m_asyncSourcePath.empty()) {
    return;
  }

  m_texture = handle;
  m_ownsTexture = false;
  m_sourcePath = m_asyncSourcePath;
  if (m_image != nullptr) {
    m_image->setTextureId(m_texture.id);
  }
  m_asyncReadySub.disconnect();
  updateLayout();
  markPaintDirty();

  if (m_asyncReadyCallback) {
    m_asyncReadyCallback();
  }
}

void Image::applyPalette() {
  const Color border = resolveColorSpec(m_border);
  if (m_image != nullptr) {
    m_image->setBorder(border, m_borderWidth);
  }
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
