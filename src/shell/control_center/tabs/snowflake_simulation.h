#pragma once

#include "render/core/render_styles.h"

#include <array>
#include <cstdint>

// Per-frame simulation for the Snow effect's flake layer - a fixed pool of explicit
// flakes with real position/radius/fall-speed state (not the jittered-cell-noise field
// the first pass used, which flickered on/off for the smallest/most distant flakes: a
// per-pixel threshold test on a sub-pixel-sized shape has no way to antialias without
// supersampling). Each flake here is drawn by the shader as a proper anti-aliased circle
// with a pixel-floor minimum radius, so flake count is capped (kMaxSnowFlakes) rather than
// density scaling into sub-pixel territory.
class SnowflakeSimulation {
public:
  void update(float deltaSeconds);
  void reset();

  [[nodiscard]] const std::array<SnowFlake, kMaxSnowFlakes>& flakes() const noexcept { return m_flakes; }
  [[nodiscard]] int flakeCount() const noexcept { return kMaxSnowFlakes; }

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

  std::array<SnowFlake, kMaxSnowFlakes> m_flakes{};
};
