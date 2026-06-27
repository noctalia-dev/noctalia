#include "render/backend/gles_texture_manager.h"

#include "core/log.h"
#include "render/core/image_decoder.h"
#include "render/core/image_file_loader.h"
#include "render/core/image_source_log.h"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <cstring>
#include <vector>

namespace {

  constexpr Logger kLog("texture");

#ifndef GL_BGRA_EXT
  constexpr GLenum GL_BGRA_EXT = 0x80E1;
#endif

  [[nodiscard]] GLuint toGlesTexture(TextureId id) noexcept { return static_cast<GLuint>(id.value()); }

  [[nodiscard]] GLenum toGlesFormat(TextureDataFormat format) noexcept {
    switch (format) {
    case TextureDataFormat::Alpha:
      return GL_ALPHA;
    case TextureDataFormat::LuminanceAlpha:
      return GL_LUMINANCE_ALPHA;
    case TextureDataFormat::Rgba:
      return GL_RGBA;
    }
    return GL_RGBA;
  }

  [[nodiscard]] GLint toGlesFilter(TextureFilter filter) noexcept {
    return filter == TextureFilter::Nearest ? GL_NEAREST : GL_LINEAR;
  }

  [[nodiscard]] GLint toGlesMipmapFilter(TextureFilter filter) noexcept {
    return filter == TextureFilter::Nearest ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
  }

} // namespace

TextureHandle GlesTextureManager::decodeEncodedRaster(
    const std::uint8_t* data, std::size_t size, const std::string* debugPath, bool mipmap
) {
  if (data == nullptr || size == 0) {
    return {};
  }

  if (auto decoded = decodeRasterImage(data, size)) {
    return uploadRgba(decoded->pixels.data(), decoded->width, decoded->height, mipmap);
  } else if (debugPath != nullptr) {
    kLog.warn("failed to decode image: {} ({})", ImageSourceLog::describe(*debugPath), decoded.error());
  }

  return {};
}

GlesTextureManager::~GlesTextureManager() { cleanup(); }

TextureHandle GlesTextureManager::loadFromFile(const std::string& path, int targetSize, bool mipmap) {
  auto loaded = loadImageFile(path, targetSize);
  if (!loaded) {
    kLog.warn("failed to load image: {} ({})", ImageSourceLog::describe(path), loaded.error());
    return {};
  }

  return loadFromRgba(loaded->rgba.data(), loaded->width, loaded->height, mipmap);
}

TextureHandle GlesTextureManager::loadFromEncodedBytes(const std::uint8_t* data, std::size_t size, bool mipmap) {
  return decodeEncodedRaster(data, size, nullptr, mipmap);
}

TextureHandle GlesTextureManager::loadFromRgba(const std::uint8_t* data, int width, int height, bool mipmap) {
  if (data == nullptr || width <= 0 || height <= 0) {
    return {};
  }
  return uploadRgba(data, width, height, mipmap);
}

TextureHandle GlesTextureManager::loadFromPixels(
    const std::uint8_t* data, int width, int height, TextureDataFormat format, TextureFilter filter, bool mipmap
) {
  if (data == nullptr || width <= 0 || height <= 0) {
    return {};
  }
  return uploadPixels(data, width, height, format, filter, mipmap);
}

TextureHandle GlesTextureManager::createEmpty(int width, int height, TextureDataFormat format, TextureFilter filter) {
  if (width <= 0 || height <= 0) {
    return {};
  }
  return uploadPixels(nullptr, width, height, format, filter, false);
}

TextureHandle GlesTextureManager::loadFromRaw(
    const std::uint8_t* data, std::size_t size, int width, int height, int stride, PixmapFormat format, bool mipmap
) {
  if (data == nullptr || size == 0 || width <= 0 || height <= 0) {
    return {};
  }

  const std::size_t channels = (format == PixmapFormat::RGB || format == PixmapFormat::BGR) ? 3U : 4U;
  const auto widthSize = static_cast<std::size_t>(width);
  const auto heightSize = static_cast<std::size_t>(height);
  const std::size_t minStride = widthSize * channels;
  const std::size_t actualStride = stride > 0 ? static_cast<std::size_t>(stride) : minStride;
  if (actualStride < minStride) {
    kLog.warn("raw pixmap stride too small: width={} channels={} stride={}", width, channels, stride);
    return {};
  }

  const std::size_t requiredSize = (heightSize - 1U) * actualStride + minStride;
  if (size < requiredSize) {
    kLog.warn(
        "raw pixmap buffer too small: width={} height={} stride={} have={} need={}", width, height, stride, size,
        requiredSize
    );
    return {};
  }

  const std::size_t widthBytes4 = static_cast<std::size_t>(width) * 4U;

  if (format == PixmapFormat::RGBA && actualStride == widthBytes4) {
    return uploadRgba(data, width, height, mipmap);
  }

  if (format == PixmapFormat::BGRA && m_hasBgraExt) {
    if (actualStride == widthBytes4) {
      return uploadBgra(data, width, height, mipmap);
    }
    std::vector<std::uint8_t> tight(widthBytes4 * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
      const auto row = static_cast<std::size_t>(y);
      std::memcpy(tight.data() + row * widthBytes4, data + row * actualStride, widthBytes4);
    }
    return uploadBgra(tight.data(), width, height, mipmap);
  }

  if (format == PixmapFormat::RGBA) {
    std::vector<std::uint8_t> tight(widthBytes4 * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
      const auto row = static_cast<std::size_t>(y);
      std::memcpy(tight.data() + row * widthBytes4, data + row * actualStride, widthBytes4);
    }
    return uploadRgba(tight.data(), width, height, mipmap);
  }

  const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  std::vector<std::uint8_t> rgba(pixelCount * 4);

  for (int y = 0; y < height; ++y) {
    const auto row = static_cast<std::size_t>(y);
    const std::uint8_t* srcRow = data + row * actualStride;
    std::uint8_t* dstRow = rgba.data() + row * widthSize * 4U;

    for (int x = 0; x < width; ++x) {
      const std::uint8_t* s = srcRow + (static_cast<std::size_t>(x) * channels);
      std::uint8_t* d = dstRow + static_cast<std::size_t>(x) * 4U;

      switch (format) {
      case PixmapFormat::BGRA:
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = s[3];
        break;
      case PixmapFormat::ARGB:
        d[0] = s[1];
        d[1] = s[2];
        d[2] = s[3];
        d[3] = s[0];
        break;
      case PixmapFormat::RGB:
        d[0] = s[0];
        d[1] = s[1];
        d[2] = s[2];
        d[3] = 255;
        break;
      case PixmapFormat::BGR:
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = 255;
        break;
      default:
        break;
      }
    }
  }

  return uploadRgba(rgba.data(), width, height, mipmap);
}

void GlesTextureManager::unload(TextureHandle& handle) {
  if (handle.id != 0) {
    GLuint texture = toGlesTexture(handle.id);
    glDeleteTextures(1, &texture);
    std::erase(m_textures, handle.id);
    handle = {};
  }
}

bool GlesTextureManager::replace(
    TextureHandle& handle, const std::uint8_t* data, int width, int height, TextureDataFormat format,
    TextureFilter filter, bool mipmap
) {
  TextureHandle next = loadFromPixels(data, width, height, format, filter, mipmap);
  if (next.id == 0) {
    return false;
  }
  unload(handle);
  handle = next;
  return true;
}

bool GlesTextureManager::updateSubImage(
    TextureHandle& handle, const std::uint8_t* data, int x, int y, int width, int height, TextureDataFormat format
) {
  if (handle.id == 0
      || data == nullptr
      || x < 0
      || y < 0
      || width <= 0
      || height <= 0
      || x + width > handle.width
      || y + height > handle.height) {
    return false;
  }

  const GLenum glFormat = toGlesFormat(format);
  glBindTexture(GL_TEXTURE_2D, toGlesTexture(handle.id));
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, glFormat, GL_UNSIGNED_BYTE, data);
  return true;
}

void GlesTextureManager::cleanup() {
  if (!m_textures.empty()) {
    std::vector<GLuint> textures;
    textures.reserve(m_textures.size());
    for (TextureId texture : m_textures) {
      textures.push_back(toGlesTexture(texture));
    }
    glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
    m_textures.clear();
  }
}

void GlesTextureManager::probeExtensions() {
  const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  if (ext != nullptr && std::strstr(ext, "GL_EXT_texture_format_BGRA8888") != nullptr) {
    m_hasBgraExt = true;
  }
}

TextureHandle GlesTextureManager::uploadBgra(const std::uint8_t* data, int width, int height, bool mipmap) {
  mipmap = mipmap && globalMipmapsEnabled();
  GLuint tex = 0;
  glGenTextures(1, &tex);
  if (tex == 0) {
    kLog.warn("glGenTextures failed for {}x{} BGRA texture", width, height);
    return {};
  }
  glBindTexture(GL_TEXTURE_2D, tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, width, height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, data);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (mipmap) {
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  m_textures.emplace_back(tex);
  return TextureHandle{.id = TextureId{tex}, .width = width, .height = height};
}

TextureHandle GlesTextureManager::uploadRgba(const std::uint8_t* data, int width, int height, bool mipmap) {
  return uploadPixels(data, width, height, TextureDataFormat::Rgba, TextureFilter::Linear, mipmap);
}

TextureHandle GlesTextureManager::uploadPixels(
    const std::uint8_t* data, int width, int height, TextureDataFormat format, TextureFilter filter, bool mipmap
) {
  mipmap = mipmap && globalMipmapsEnabled();
  GLuint tex = 0;
  glGenTextures(1, &tex);
  if (tex == 0) {
    kLog.warn("glGenTextures failed for {}x{} texture", width, height);
    return {};
  }
  glBindTexture(GL_TEXTURE_2D, tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  const GLenum glFormat = toGlesFormat(format);
  glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(glFormat), width, height, 0, glFormat, GL_UNSIGNED_BYTE, data);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (mipmap) {
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, toGlesMipmapFilter(filter));
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, toGlesFilter(filter));
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, toGlesFilter(filter));

  m_textures.emplace_back(tex);
  return TextureHandle{.id = TextureId{tex}, .width = width, .height = height};
}
