#pragma once

#include "render/backend/render_backend.h"
#include "render/core/texture_handle.h"

#include <cstdint>
#include <functional>
#include <memory>

class CachedLayer {
public:
  CachedLayer() = default;
  ~CachedLayer() = default;

  CachedLayer(const CachedLayer&) = delete;
  CachedLayer& operator=(const CachedLayer&) = delete;
  CachedLayer(CachedLayer&&) noexcept = default;
  CachedLayer& operator=(CachedLayer&&) noexcept = default;

  using RenderCallback = std::function<void(RenderFramebuffer& target)>;

  void resize(RenderBackend& backend, std::uint32_t width, std::uint32_t height);

  void invalidate() noexcept { m_dirty = true; }

  TextureId ensure(const RenderCallback& renderFn);

  [[nodiscard]] TextureId texture() const noexcept;
  [[nodiscard]] RenderFramebuffer* scratch();
  [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
  [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }
  [[nodiscard]] bool dirty() const noexcept { return m_dirty; }
  [[nodiscard]] bool valid() const noexcept { return m_framebuffer != nullptr && m_framebuffer->valid(); }

  void destroy();
  void abandon() noexcept;

private:
  RenderBackend* m_backend = nullptr;
  std::unique_ptr<RenderFramebuffer> m_framebuffer;
  std::unique_ptr<RenderFramebuffer> m_scratch;
  std::uint32_t m_width = 0;
  std::uint32_t m_height = 0;
  bool m_dirty = true;
};
