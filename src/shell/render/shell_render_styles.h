#pragma once

#include "render/core/color.h"
#include "widget_render/widget_render_styles.h"

#include <cstdint>

struct CountdownRingStyle {
  Color color{};
  float thickness = 6.0F;
  float progress = 1.0F;
};

enum class ScreenCornerPosition : std::uint8_t {
  TopLeft,
  TopRight,
  BottomRight,
  BottomLeft,
};

struct ScreenCornerStyle {
  Color color = rgba(0.0F, 0.0F, 0.0F, 1.0F);
  ScreenCornerPosition position = ScreenCornerPosition::TopLeft;
  float exponent = 4.0F;
};

constexpr bool operator==(const ScreenCornerStyle& lhs, const ScreenCornerStyle& rhs) noexcept {
  return lhs.color == rhs.color && lhs.position == rhs.position && lhs.exponent == rhs.exponent;
}

enum class WeatherEffectType : std::uint8_t { None, Sun, Snow, Rain, Cloud, Fog, Stars };

// Particle capacities of the Rain (glass droplets) and Snow (flakes) layers. The shaders loop
// over these as literals, so kRainFragment and kSnowFragment must match.
inline constexpr int kMaxRainDrops = 24;
inline constexpr int kMaxSnowFlakes = 60;
inline constexpr int kMaxWeatherEffectParticles = kMaxSnowFlakes;

// One Rain drop or Snow flake. Position and radius are in the effect's local uv space [0,1];
// weight in [0,1] is a Rain drop's normalized momentum (trail length) and unused by Snow.
struct WeatherEffectParticle {
  float x = 0.0F;
  float y = 0.0F;
  float radius = 0.0F;
  float weight = 0.0F;
};

struct WeatherEffectStyle {
  WeatherEffectType type = WeatherEffectType::None;
  float time = 0.0F;
  float radius = 0.0F;
  Color bgColor{};
  // Theme accent (ColorRole::Primary) tinting the Fog and Cloud palettes.
  Color accentColor{};
  // Particles in WeatherEffectNode's particle texture; read by the Rain and Snow shaders.
  int particleCount = 0;
};
