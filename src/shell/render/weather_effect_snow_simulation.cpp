#include "shell/render/weather_effect_snow_simulation.h"

#include <cmath>

namespace {
  constexpr float kMinRadius = 0.005F;
  constexpr float kMaxRadius = 0.028F;
  constexpr float kMinFallSpeed = 0.045F;
  constexpr float kMaxFallSpeed = 0.16F;
  constexpr float kSwaySpeedMin = 0.4F;
  constexpr float kSwaySpeedMax = 1.6F;
  constexpr float kSwayAmountMin = 0.01F;
  constexpr float kSwayAmountMax = 0.045F;
} // namespace

float WeatherEffectSnowSimulation::rand01() {
  m_rngState ^= m_rngState << 13;
  m_rngState ^= m_rngState >> 17;
  m_rngState ^= m_rngState << 5;
  return static_cast<float>(m_rngState) / static_cast<float>(0xFFFFFFFFU);
}

void WeatherEffectSnowSimulation::respawn(SimFlake& flake, bool initialScatter) {
  // Product of two uniforms biases toward small, distant flakes with occasional large ones.
  const float sizeT = rand01() * rand01();
  // Bigger flakes fall faster as a depth cue.
  flake.radius = kMinRadius + (kMaxRadius - kMinRadius) * sizeT;
  flake.fallSpeed = kMinFallSpeed + (kMaxFallSpeed - kMinFallSpeed) * sizeT;
  flake.swayPhase = rand01() * 6.2831853F;
  flake.swaySpeed = kSwaySpeedMin + rand01() * (kSwaySpeedMax - kSwaySpeedMin);
  flake.swayAmount = kSwayAmountMin + rand01() * (kSwayAmountMax - kSwayAmountMin);
  flake.x = rand01();
  // Initial fill scatters flakes over the whole card; respawns start just above the top edge.
  flake.y = initialScatter ? rand01() : -flake.radius * (0.5F + rand01());
}

void WeatherEffectSnowSimulation::reset() {
  m_sim = {};
  m_windPhase = 0.0F;
  for (auto& flake : m_sim) {
    respawn(flake, /*initialScatter=*/true);
  }
  m_particles = {};
}

void WeatherEffectSnowSimulation::update(float deltaSeconds) {
  m_windPhase += deltaSeconds;
  // Slow wind gust from two mismatched sines, so the snowfall leans one way and calms again.
  const float windGust = std::sin(m_windPhase * 0.15F) * 0.035F + std::sin(m_windPhase * 0.067F + 1.7F) * 0.025F;

  for (int i = 0; i < kMaxSnowFlakes; ++i) {
    SimFlake& flake = m_sim[static_cast<std::size_t>(i)];
    if (flake.radius <= 0.0F) {
      respawn(flake, /*initialScatter=*/false);
    }

    flake.swayPhase += flake.swaySpeed * deltaSeconds;
    // Wind pushes bigger, closer flakes further.
    const float windWeight = flake.radius / kMaxRadius;
    flake.x += (std::sin(flake.swayPhase) * flake.swayAmount + windGust * windWeight) * deltaSeconds;
    flake.y += flake.fallSpeed * deltaSeconds;

    if (flake.x < 0.0F) {
      flake.x += 1.0F;
    } else if (flake.x > 1.0F) {
      flake.x -= 1.0F;
    }

    if (flake.y - flake.radius > 1.0F) {
      respawn(flake, /*initialScatter=*/false);
    }

    WeatherEffectParticle& out = m_particles[static_cast<std::size_t>(i)];
    out.x = flake.x;
    out.y = flake.y;
    out.radius = flake.radius;
  }
}
