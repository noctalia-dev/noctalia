#include "shell/control_center/tabs/rain_droplet_simulation.h"

#include <cmath>

namespace {
  constexpr float kMinRadius = 0.018F;
  constexpr float kMaxRadius = 0.038F;
  // Raised from the first pass's 1.5/s, which left only ~3 drops alive at once (at
  // kGravity=0.4 a drop clears the card in well under a second, so spawns have to
  // outpace that to read as a populated pane rather than a handful of stragglers).
  constexpr float kSpawnRatePerSecond = 5.0F;
  constexpr float kBaseMomentum = 0.05F;
  constexpr float kGravity = 0.4F;
  constexpr float kMaxMomentum = 0.9F;
  constexpr float kCollisionRadiusFactor = 0.85F;
  constexpr float kMergeAreaLossFactor = 0.8F;
} // namespace

float RainDropletSimulation::rand01() {
  // xorshift32 - deterministic, no <random> allocation/state needed per tick.
  m_rngState ^= m_rngState << 13;
  m_rngState ^= m_rngState >> 17;
  m_rngState ^= m_rngState << 5;
  return static_cast<float>(m_rngState) / static_cast<float>(0xFFFFFFFFU);
}

void RainDropletSimulation::reset() {
  m_sim = {};
  m_count = 0;
  m_spawnAccumulator = 0.0F;
  m_drops = {};
}

void RainDropletSimulation::trySpawn(float deltaSeconds) {
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

void RainDropletSimulation::advance(float deltaSeconds) {
  for (int i = 0; i < m_count; ++i) {
    SimDrop& drop = m_sim[static_cast<std::size_t>(i)];
    if (drop.killed) {
      continue;
    }
    // Momentum grows over time instead of falling at a constant speed - an eased,
    // accelerating descent rather than linear motion.
    drop.momentum = std::min(kMaxMomentum, drop.momentum + kGravity * deltaSeconds);
    drop.y += drop.momentum * deltaSeconds;
    if (drop.y - drop.r > 1.0F) {
      drop.killed = true;
    }
  }
}

void RainDropletSimulation::mergeCollisions() {
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
      // Area conservation: the bigger drop absorbs the smaller one's area (with a loss
      // factor), gains its momentum as an impact boost, and the smaller drop is removed -
      // a real state merge, not a shader-side blend of two still-separate drops.
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

void RainDropletSimulation::compactAndPublish() {
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
    RainDrop& out = m_drops[static_cast<std::size_t>(i)];
    out.x = drop.x;
    out.y = drop.y;
    out.radius = drop.r;
    // Momentum normalized to [0,1] - the shader uses this to length a trailing "comet
    // tail" of shrinking domes behind each drop, matching the lab's runner-drop look.
    out.depth = std::min(1.0F, drop.momentum / kMaxMomentum);
  }
}

void RainDropletSimulation::update(float deltaSeconds) {
  trySpawn(deltaSeconds);
  advance(deltaSeconds);
  mergeCollisions();
  compactAndPublish();
}
