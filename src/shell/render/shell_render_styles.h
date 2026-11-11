#pragma once

#include "render/core/color.h"

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

enum class AudioSpectrumOrientation : std::uint8_t {
  Horizontal,
  Vertical,
};

// Wave mode renders from this many control points, so wave visualizers request exactly this
// many spectrum bands and ignore their `bands` setting.
inline constexpr int kAudioSpectrumWaveBands = 32;

struct AudioSpectrumStyle {
  Color color1{};
  Color color2{};
  AudioSpectrumOrientation orientation = AudioSpectrumOrientation::Horizontal;
  bool mirrored = false;
  bool reversed = false;
  bool centered = false;
  bool wave = false;
};

constexpr bool operator==(const AudioSpectrumStyle& lhs, const AudioSpectrumStyle& rhs) noexcept {
  return lhs.color1 == rhs.color1
      && lhs.color2 == rhs.color2
      && lhs.orientation == rhs.orientation
      && lhs.mirrored == rhs.mirrored
      && lhs.reversed == rhs.reversed
      && lhs.centered == rhs.centered
      && lhs.wave == rhs.wave;
}

enum class FancyAudioVisualizerMode : std::uint8_t {
  Bars,
  Wave,
  Rings,
  BarsRings,
  WaveRings,
  All,
};

struct FancyAudioVisualizerStyle {
  Color primaryColor = rgba(0.0F, 0.0F, 0.0F, 1.0F);
  Color secondaryColor = rgba(0.0F, 0.0F, 0.0F, 1.0F);
  FancyAudioVisualizerMode mode = FancyAudioVisualizerMode::BarsRings;
  float time = 0.0F;
  float sensitivity = 1.5F;
  float rotationSpeed = 0.5F;
  float barWidth = 0.6F;
  float ringOpacity = 0.8F;
  float bloomIntensity = 0.5F;
  float waveThickness = 1.0F;
  float innerDiameter = 0.7F;
  float cornerRadius = 12.0F;
};

constexpr bool operator==(const FancyAudioVisualizerStyle& lhs, const FancyAudioVisualizerStyle& rhs) noexcept {
  return lhs.primaryColor == rhs.primaryColor
      && lhs.secondaryColor == rhs.secondaryColor
      && lhs.mode == rhs.mode
      && lhs.time == rhs.time
      && lhs.sensitivity == rhs.sensitivity
      && lhs.rotationSpeed == rhs.rotationSpeed
      && lhs.barWidth == rhs.barWidth
      && lhs.ringOpacity == rhs.ringOpacity
      && lhs.bloomIntensity == rhs.bloomIntensity
      && lhs.waveThickness == rhs.waveThickness
      && lhs.innerDiameter == rhs.innerDiameter
      && lhs.cornerRadius == rhs.cornerRadius;
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

struct GraphStyle {
  Color lineColor1{};
  float count1 = 0.0F;
  float scroll1 = 1.0F;

  Color lineColor2{};
  float count2 = 0.0F;
  float scroll2 = 1.0F;

  Color lineColor3{};
  float count3 = 0.0F;
  float scroll3 = 1.0F;

  float lineWidth = 1.5F;
  float graphFillOpacity = 0.15F;
  float aaSize = 0.5F;
};
