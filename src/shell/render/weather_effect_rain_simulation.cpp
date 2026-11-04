#include "shell/render/weather_effect_rain_simulation.h"

#include <cmath>

namespace {
  constexpr float kMinRadius = 0.018F;
  constexpr float kMaxRadius = 0.038F;
  // A drop clears the card in under a second, so spawns must outpace that to keep the pane populated.
  constexpr float kSpawnRatePerSecond = 5.0F;
  constexpr float kBaseMomentum = 0.05F;
  constexpr float kGravity = 0.4F;
  constexpr float kMaxMomentum = 0.9F;
  constexpr float kCollisionRadiusFactor = 0.85F;
  constexpr float kMergeAreaLossFactor = 0.8F;
} // namespace

float WeatherEffectRainSimulation::rand01() {
  // xorshift32: deterministic, no <random> state needed per tick.
  m_rngState ^= m_rngState << 13;
  m_rngState ^= m_rngState >> 17;
  m_rngState ^= m_rngState << 5;
  return static_cast<float>(m_rngState) / static_cast<float>(0xFFFFFFFFU);
}

void WeatherEffectRainSimulation::reset() {
  m_sim = {};
  m_count = 0;
  m_spawnAccumulator = 0.0F;
  m_particles = {};
}

void WeatherEffectRainSimulation::trySpawn(float deltaSeconds) {
  m_spawnAccumulator += kSpawnRatePerSecond * deltaSeconds;
  while (m_spawnAccumulator >= 1.0F && m_count < kMaxRainDrops) {
    m_spawnAccumulator -= 1.0F;
    const float sizeT = rand01();
    SimDrop& drop = m_sim[static_cast<std::size_t>(m_count)];
    drop = SimDrop{};
    drop.x = rand01();
    drop.y = -kMaxRadius * rand01();
    drop.r = kMinRadius + (kMaxRadius - kMinRadius) * sizeT * sizeT * sizeT;
    drop.momentum = kBaseMomentum;
    ++m_count;
  }
}

void WeatherEffectRainSimulation::advance(float deltaSeconds) {
  for (int i = 0; i < m_count; ++i) {
    SimDrop& drop = m_sim[static_cast<std::size_t>(i)];
    if (drop.killed) {
      continue;
    }
    // Momentum grows over time, so drops accelerate as they fall.
    drop.momentum = std::min(kMaxMomentum, drop.momentum + kGravity * deltaSeconds);
    drop.y += drop.momentum * deltaSeconds;
    if (drop.y - drop.r > 1.0F) {
      drop.killed = true;
    }
  }
}

void WeatherEffectRainSimulation::mergeCollisions() {
  for (int i = 0; i < m_count; ++i) {
    SimDrop& a = m_sim[static_cast<std::size_t>(i)];
    if (a.killed) {
      continue;
    }
    for (int j = i + 1; j < m_count; ++j) {
      SimDrop& b = m_sim[static_cast<std::size_t>(j)];
      if (b.killed) {
        continue;
      }
      SimDrop& big = (a.r >= b.r) ? a : b;
      SimDrop& small = (a.r >= b.r) ? b : a;
      const float dx = big.x - small.x;
      const float dy = big.y - small.y;
      const float dist = std::sqrt(dx * dx + dy * dy);
      if (dist >= (big.r + small.r) * kCollisionRadiusFactor) {
        continue;
      }
      // The bigger drop absorbs the smaller one's area (with a loss factor) and gets an
      // impact boost to its momentum; the smaller drop is removed.
      const float areaBig = big.r * big.r;
      const float areaSmall = small.r * small.r;
      float targetR = std::sqrt(areaBig + areaSmall * kMergeAreaLossFactor);
      targetR = std::min(targetR, kMaxRadius);
      big.r = targetR;
      big.momentum = std::min(kMaxMomentum, std::max(big.momentum, small.momentum) + 0.05F);
      small.killed = true;
      if (a.killed || b.killed) {
        break;
      }
    }
  }
}

void WeatherEffectRainSimulation::compactAndPublish() {
  int writeIdx = 0;
  for (int i = 0; i < m_count; ++i) {
    const SimDrop& drop = m_sim[static_cast<std::size_t>(i)];
    if (drop.killed) {
      continue;
    }
    m_sim[static_cast<std::size_t>(writeIdx)] = drop;
    ++writeIdx;
  }
  m_count = writeIdx;

  for (int i = 0; i < m_count; ++i) {
    const SimDrop& drop = m_sim[static_cast<std::size_t>(i)];
    WeatherEffectParticle& out = m_particles[static_cast<std::size_t>(i)];
    out.x = drop.x;
    out.y = drop.y;
    out.radius = drop.r;
    // Normalized momentum; the shader uses it to length each drop's trail.
    out.weight = std::min(1.0F, drop.momentum / kMaxMomentum);
  }
}

void WeatherEffectRainSimulation::update(float deltaSeconds) {
  trySpawn(deltaSeconds);
  advance(deltaSeconds);
  mergeCollisions();
  compactAndPublish();
}
