#pragma once

#include "render/core/render_styles.h"

#include <array>
#include <cstdint>

// Small per-frame physics simulation for the Rain effect's glass-droplet layer: drops
// spawn near the top, fall with eased (increasing) momentum, and merge via area
// conservation when they get close - ported from the prototyping lab's updateDrops/
// updateRain logic (openspec/changes/weather-rain-effect-v2/lab/weather-shader-lab.html),
// simplified to the ~kMaxRainDrops capacity the native uniform array can carry. Position
// is in the effect's local uv space [0,1] (matching v_uv), independent of aspect ratio -
// the Rain shader applies the aspect correction itself.
class RainDropletSimulation {
public:
  void update(float deltaSeconds);
  void reset();

  [[nodiscard]] const std::array<RainDrop, kMaxRainDrops>& drops() const noexcept { return m_drops; }
  [[nodiscard]] int dropCount() const noexcept { return m_count; }

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

  std::array<RainDrop, kMaxRainDrops> m_drops{};
};
