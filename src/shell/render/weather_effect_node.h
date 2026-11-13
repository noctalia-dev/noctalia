#pragma once

#include "render/core/color.h"
#include "render/core/texture_handle.h"
#include "render/scene/custom_draw_node.h"
#include "shell/render/shell_render_styles.h"

#include <array>
#include <cstdint>
#include <span>

class TextureManager;

class WeatherEffectNode : public CustomDrawNode {
public:
  // Two RGBA8 texels per particle: (x, y) then (radius, weight), each value 16-bit as hi/lo
  // bytes over the ranges below. particleAt() in weather_effect_common_glsl.h must match all four.
  static constexpr int kParticleTextureWidth = kMaxWeatherEffectParticles * 2;
  static constexpr float kParticleYMin = -0.25F;
  static constexpr float kParticleYMax = 1.25F;
  static constexpr float kParticleRadiusMax = 0.0625F;

  WeatherEffectNode() = default;
  ~WeatherEffectNode() override;

  WeatherEffectNode(const WeatherEffectNode&) = delete;
  WeatherEffectNode& operator=(const WeatherEffectNode&) = delete;

  void setEffectType(WeatherEffectType type) {
    if (m_style.type == type) {
      return;
    }
    m_style.type = type;
    markPaintDirty();
  }

  void setTime(float time) {
    m_style.time = time;
    markPaintDirty();
  }

  void setRadius(float radius) {
    if (m_style.radius == radius) {
      return;
    }
    m_style.radius = radius;
    markPaintDirty();
  }

  void setBgColor(const Color& color) {
    if (m_style.bgColor == color) {
      return;
    }
    m_style.bgColor = color;
    markPaintDirty();
  }

  void setAccentColor(const Color& color) {
    if (m_style.accentColor == color) {
      return;
    }
    m_style.accentColor = color;
    markPaintDirty();
  }

  // Uploads the Rain or Snow particle layer; must run with the render target current.
  // Particles past kMaxWeatherEffectParticles are dropped.
  void setParticles(TextureManager& textures, std::span<const WeatherEffectParticle> particles);

  void draw(RenderBackend& backend, const CustomDrawContext& context) const override;

private:
  void doInvalidateGpuResources(Renderer& renderer) override;

  WeatherEffectStyle m_style;
  TextureManager* m_textureManager = nullptr;
  TextureHandle m_particleTexture;
  std::array<std::uint8_t, static_cast<std::size_t>(kParticleTextureWidth) * 4U> m_particlePixels{};
};
