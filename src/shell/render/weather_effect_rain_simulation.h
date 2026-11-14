#pragma once

#include "shell/render/shell_render_styles.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// Per-frame simulation for the Rain effect's glass-droplet layer: drops spawn near the top,
// fall with increasing momentum, and merge by area when they overlap. Positions are in the
// effect's local uv space [0,1]; the shader applies the aspect correction.
class WeatherEffectRainSimulation {
public:
  void update(float deltaSeconds);
  void reset();

  [[nodiscard]] std::span<const WeatherEffectParticle> particles() const noexcept {
    return {m_particles.data(), static_cast<std::size_t>(m_count)};
  }

private:
  struct SimDrop {
    float x = 0.0F;
    float y = 0.0F;
    float r = 0.0F;
    float momentum = 0.0F;
    bool killed = false;
  };

  [[nodiscard]] float rand01();
  void trySpawn(float deltaSeconds);
  void advance(float deltaSeconds);
  void mergeCollisions();
  void compactAndPublish();

  std::array<SimDrop, kMaxRainDrops> m_sim{};
  int m_count = 0;
  float m_spawnAccumulator = 0.0F;
  std::uint32_t m_rngState = 0x9E3779B9U;

  std::array<WeatherEffectParticle, kMaxRainDrops> m_particles{};
};
