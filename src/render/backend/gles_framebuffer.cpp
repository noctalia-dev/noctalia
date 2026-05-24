#include "render/backend/gles_framebuffer.h"

#include <utility>

GlesFramebuffer::~GlesFramebuffer() { destroy(); }

GlesFramebuffer::GlesFramebuffer(GlesFramebuffer&& other) noexcept
    : m_textures(std::exchange(other.m_textures, nullptr)), m_id(std::exchange(other.m_id, 0)),
      m_color(std::exchange(other.m_color, {})), m_width(std::exchange(other.m_width, 0)),
      m_height(std::exchange(other.m_height, 0)) {}

GlesFramebuffer& GlesFramebuffer::operator=(GlesFramebuffer&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  destroy();
  m_textures = std::exchange(other.m_textures, nullptr);
  m_id = std::exchange(other.m_id, 0);
  m_color = std::exchange(other.m_color, {});
  m_width = std::exchange(other.m_width, 0);
  m_height = std::exchange(other.m_height, 0);
  return *this;
}

bool GlesFramebuffer::create(TextureManager& textures, std::uint32_t width, std::uint32_t height) {
  destroy();
  if (width == 0 || height == 0) {
    return false;
  }

  m_textures = &textures;
  m_color = textures.createEmpty(
      static_cast<int>(width), static_cast<int>(height), TextureDataFormat::Rgba, TextureFilter::Linear
  );
  if (m_color.id == 0) {
    m_textures = nullptr;
    return false;
  }

  glGenFramebuffers(1, &m_id);
  if (m_id == 0) {
    destroy();
    return false;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, m_id);
  glFramebufferTexture2D(
      GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, static_cast<GLuint>(m_color.id.value()), 0
  );
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    destroy();
    return false;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  m_width = width;
  m_height = height;
  return true;
}

void GlesFramebuffer::destroy() {
  if (m_id != 0) {
    glDeleteFramebuffers(1, &m_id);
    m_id = 0;
  }

  if (m_textures != nullptr) {
    m_textures->unload(m_color);
  }
  m_textures = nullptr;
  m_width = 0;
  m_height = 0;
}

void GlesFramebuffer::bind() const { glBindFramebuffer(GL_FRAMEBUFFER, m_id); }

void GlesFramebuffer::bindDefault() { glBindFramebuffer(GL_FRAMEBUFFER, 0); }
