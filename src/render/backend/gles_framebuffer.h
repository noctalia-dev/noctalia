#pragma once

#include "render/backend/render_backend.h"
#include "render/core/texture_handle.h"

#include <GLES2/gl2.h>
#include <cstdint>

class GlesFramebuffer final : public RenderFramebuffer {
public:
  GlesFramebuffer() = default;
  ~GlesFramebuffer() override;

  GlesFramebuffer(const GlesFramebuffer&) = delete;
  GlesFramebuffer& operator=(const GlesFramebuffer&) = delete;

  GlesFramebuffer(GlesFramebuffer&& other) noexcept;
  GlesFramebuffer& operator=(GlesFramebuffer&& other) noexcept;

  [[nodiscard]] bool create(TextureManager& textures, std::uint32_t width, std::uint32_t height);
  void destroy();

  void bind() const;
  static void bindDefault();

  [[nodiscard]] bool valid() const noexcept override { return m_id != 0 && m_color.id != 0; }
  [[nodiscard]] GLuint id() const noexcept { return m_id; }
  [[nodiscard]] TextureId colorTexture() const noexcept override { return m_color.id; }
  [[nodiscard]] std::uint32_t width() const noexcept override { return m_width; }
  [[nodiscard]] std::uint32_t height() const noexcept override { return m_height; }

private:
  TextureManager* m_textures = nullptr;
  GLuint m_id = 0;
  TextureHandle m_color;
  std::uint32_t m_width = 0;
  std::uint32_t m_height = 0;
};
