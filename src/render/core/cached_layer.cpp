#include "render/core/cached_layer.h"

void CachedLayer::resize(RenderBackend& backend, std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0) {
    return;
  }

  m_backend = &backend;

  if (m_width == width && m_height == height && valid()) {
    return;
  }

  m_width = width;
  m_height = height;
  m_framebuffer = backend.createFramebuffer(width, height);
  m_scratch.reset();
  m_dirty = true;
}

TextureId CachedLayer::ensure(const RenderCallback& renderFn) {
  if (!m_dirty || !valid()) {
    return texture();
  }

  renderFn(*m_framebuffer);
  m_dirty = false;
  return texture();
}

TextureId CachedLayer::texture() const noexcept {
  if (m_framebuffer != nullptr && m_framebuffer->valid()) {
    return m_framebuffer->colorTexture();
  }
  return {};
}

RenderFramebuffer* CachedLayer::scratch() {
  if (m_scratch != nullptr && m_scratch->valid() && m_scratch->width() == m_width && m_scratch->height() == m_height) {
    return m_scratch.get();
  }

  if (m_backend == nullptr || m_width == 0 || m_height == 0) {
    return nullptr;
  }

  m_scratch = m_backend->createFramebuffer(m_width, m_height);
  if (m_scratch == nullptr || !m_scratch->valid()) {
    m_scratch.reset();
    return nullptr;
  }
  return m_scratch.get();
}

void CachedLayer::destroy() {
  m_framebuffer.reset();
  m_scratch.reset();
  m_backend = nullptr;
  m_width = 0;
  m_height = 0;
  m_dirty = true;
}

void CachedLayer::abandon() noexcept {
  if (m_framebuffer != nullptr) {
    m_framebuffer->abandon();
  }
  if (m_scratch != nullptr) {
    m_scratch->abandon();
  }
  m_framebuffer.reset();
  m_scratch.reset();
  m_backend = nullptr;
  m_width = 0;
  m_height = 0;
  m_dirty = true;
}
