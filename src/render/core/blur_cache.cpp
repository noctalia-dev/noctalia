#include "render/core/blur_cache.h"

#include "render/backend/render_backend.h"

BlurCache::~BlurCache() { destroy(); }

TextureHandle BlurCache::get(
    RenderBackend& backend, TextureHandle source, std::uint32_t width, std::uint32_t height, float radius, int rounds
) {
  if (source.id == TextureId{} || width == 0 || height == 0) {
    return {};
  }

  m_backend = &backend;

  if (source.id != m_lastSourceTex || radius != m_lastRadius || rounds != m_lastRounds) {
    m_lastSourceTex = source.id;
    m_lastRadius = radius;
    m_lastRounds = rounds;
    m_layer.invalidate();
  }

  if (!backend.makeCurrentNoSurface()) {
    return {};
  }
  m_layer.resize(backend, width, height);

  m_layer.ensure([&](RenderFramebuffer& target) {
    backend.disableScissor();
    backend.bindFramebuffer(target);
    backend.setViewport(width, height);
    backend.setBlendMode(RenderBlendMode::Disabled);
    backend.clear(rgba(0.0f, 0.0f, 0.0f, 1.0f));
    backend.drawImage(
        RenderImageDraw{
            .texture = source.id,
            .surfaceWidth = static_cast<float>(width),
            .surfaceHeight = static_cast<float>(height),
            .width = static_cast<float>(width),
            .height = static_cast<float>(height),
            .fitMode = RenderImageFitMode::Cover,
            .textureWidth = static_cast<float>(source.width),
            .textureHeight = static_cast<float>(source.height),
            .transform = Mat3::translation(0.0f, static_cast<float>(height)) * Mat3::scale(1.0f, -1.0f),
        }
    );

    auto* scratch = m_layer.scratch();
    if (scratch != nullptr && radius >= 0.5f && rounds > 0) {
      for (int round = 0; round < rounds; ++round) {
        backend.bindFramebuffer(*scratch);
        backend.drawFramebufferBlur(target.colorTexture(), width, height, 1.0f, 0.0f, radius);
        backend.bindFramebuffer(target);
        backend.drawFramebufferBlur(scratch->colorTexture(), width, height, 0.0f, 1.0f, radius);
      }
    }
  });

  backend.bindDefaultFramebuffer();

  const auto tex = m_layer.texture();
  if (tex == TextureId{}) {
    return {};
  }
  return {tex, static_cast<int>(width), static_cast<int>(height)};
}

void BlurCache::destroy() {
  if (m_backend != nullptr) {
    m_backend->makeCurrentNoSurface();
  }
  m_layer.destroy();
  m_backend = nullptr;
  m_lastSourceTex = {};
  m_lastRadius = 0.0f;
  m_lastRounds = 0;
}

void BlurCache::abandon() noexcept {
  m_layer.abandon();
  m_backend = nullptr;
  m_lastSourceTex = {};
  m_lastRadius = 0.0f;
  m_lastRounds = 0;
}
