#include "shell/render/weather_effect_node.h"

#include "render/backend/render_backend.h"
#include "render/core/texture_manager.h"
#include "shell/render/programs/weather_effect_program.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace {

  // Writes `value` normalized over [lo, hi] as a 16-bit big-endian pair.
  void packUnorm16(std::uint8_t* out, float value, float lo, float hi) {
    const float t = std::clamp((value - lo) / (hi - lo), 0.0F, 1.0F);
    const auto q = static_cast<std::uint32_t>(std::lround(t * 65535.0F));
    out[0] = static_cast<std::uint8_t>(q >> 8U);
    out[1] = static_cast<std::uint8_t>(q & 0xFFU);
  }

} // namespace

WeatherEffectNode::~WeatherEffectNode() {
  if (m_textureManager != nullptr) {
    m_textureManager->unload(m_particleTexture);
  }
}

void WeatherEffectNode::setParticles(TextureManager& textures, std::span<const WeatherEffectParticle> particles) {
  if (m_textureManager != nullptr && m_textureManager != &textures) {
    m_textureManager->unload(m_particleTexture);
  }
  m_textureManager = &textures;

  const std::size_t count = std::min(particles.size(), static_cast<std::size_t>(kMaxWeatherEffectParticles));
  m_particlePixels.fill(0);
  for (std::size_t i = 0; i < count; ++i) {
    const WeatherEffectParticle& particle = particles[i];
    std::uint8_t* texel = &m_particlePixels[i * 8U];
    packUnorm16(texel, particle.x, 0.0F, 1.0F);
    packUnorm16(texel + 2, particle.y, kParticleYMin, kParticleYMax);
    packUnorm16(texel + 4, particle.radius, 0.0F, kParticleRadiusMax);
    packUnorm16(texel + 6, particle.weight, 0.0F, 1.0F);
  }

  const bool uploaded = m_particleTexture.valid()
      ? textures.updateSubImage(
            m_particleTexture, m_particlePixels.data(), 0, 0, kParticleTextureWidth, 1, TextureDataFormat::Rgba
        )
      : textures.replace(
            m_particleTexture, m_particlePixels.data(), kParticleTextureWidth, 1, TextureDataFormat::Rgba,
            TextureFilter::Nearest
        );
  m_style.particleCount = uploaded ? static_cast<int>(count) : 0;
  markPaintDirty();
}

void WeatherEffectNode::draw(RenderBackend& backend, const CustomDrawContext& context) const {
  auto style = m_style;
  style.bgColor.a *= context.opacity;
  auto& program = backend.extension<WeatherEffectProgram>();
  program.ensureInitialized();
  program.draw(
      m_particleTexture.id, context.surfaceWidth, context.surfaceHeight, context.width, context.height, style,
      context.transform
  );
}

void WeatherEffectNode::doInvalidateGpuResources(Renderer& renderer) {
  (void)renderer;
  if (m_textureManager != nullptr && m_particleTexture.valid()) {
    m_textureManager->unload(m_particleTexture);
  }
  m_particleTexture = {};
  m_style.particleCount = 0;
  markPaintDirty();
}
