#pragma once

#include "shell/render/shell_render_styles.h"

#include <array>
#include <cstdint>
#include <span>

// Per-frame simulation for the Snow effect's flake layer: a fixed pool of kMaxSnowFlakes
// flakes with position, radius, fall speed, and sway, in the effect's local uv space [0,1].
// The shader draws each flake as an anti-aliased circle with a minimum on-screen radius.
class SnowflakeSimulation {
public:
  void update(float deltaSeconds);
  void reset();

  [[nodiscard]] std::span<const EffectParticle> particles() const noexcept { return m_particles; }

private:
  struct SimFlake {
    float x = 0.0F;
    float y = 0.0F;
    float radius = 0.0F;
    float fallSpeed = 0.0F;
    float swayPhase = 0.0F;
    float swaySpeed = 0.0F;
    float swayAmount = 0.0F;
  };

  [[nodiscard]] float rand01();
  void respawn(SimFlake& flake, bool initialScatter);

  std::array<SimFlake, kMaxSnowFlakes> m_sim{};
  float m_windPhase = 0.0F;
  std::uint32_t m_rngState = 0x1E3779B9U;

  std::array<EffectParticle, kMaxSnowFlakes> m_particles{};
};
