#include "shell/control_center/tabs/snowflake_simulation.h"

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

float SnowflakeSimulation::rand01() {
  m_rngState ^= m_rngState << 13;
  m_rngState ^= m_rngState >> 17;
  m_rngState ^= m_rngState << 5;
  return static_cast<float>(m_rngState) / static_cast<float>(0xFFFFFFFFU);
}

void SnowflakeSimulation::respawn(SimFlake& flake, bool initialScatter) {
  // Squared instead of linear - biases toward more small/distant flakes and fewer
  // big/close ones (most snow reads as a fine background dusting with only occasional
  // large flakes up close), instead of an even spread that made every flake look like a
  // similar mid-size dot with no real parallax read.
  const float sizeT = rand01() * rand01();
  // Bigger flakes fall faster - a cheap but effective depth cue (closer/larger things
  // moving faster than distant/smaller ones), same idea as real parallax snowfall.
  flake.radius = kMinRadius + (kMaxRadius - kMinRadius) * sizeT;
  flake.fallSpeed = kMinFallSpeed + (kMaxFallSpeed - kMinFallSpeed) * sizeT;
  flake.swayPhase = rand01() * 6.2831853F;
  flake.swaySpeed = kSwaySpeedMin + rand01() * (kSwaySpeedMax - kSwaySpeedMin);
  flake.swayAmount = kSwayAmountMin + rand01() * (kSwayAmountMax - kSwayAmountMin);
  flake.x = rand01();
  // On first fill, scatter flakes across the whole card height so it doesn't start empty
  // and fill from the top over several seconds; on respawn (flake fell off the bottom),
  // start just above the visible area like real snow entering frame.
  flake.y = initialScatter ? rand01() : -flake.radius * (0.5F + rand01());
}

void SnowflakeSimulation::reset() {
  m_sim = {};
  m_windPhase = 0.0F;
  for (auto& flake : m_sim) {
    respawn(flake, /*initialScatter=*/true);
  }
  m_flakes = {};
}

void SnowflakeSimulation::update(float deltaSeconds) {
  m_windPhase += deltaSeconds;
  // Slow, low-frequency wind gust (two mismatched sines, so it never quite repeats) -
  // without this the only sideways motion is each flake's own small wobble, and the
  // whole snowfall never visibly leans one way for a while the way real snow does during
  // a gust, then calms again.
  const float windGust = std::sin(m_windPhase * 0.15F) * 0.035F + std::sin(m_windPhase * 0.067F + 1.7F) * 0.025F;

  for (int i = 0; i < kMaxSnowFlakes; ++i) {
    SimFlake& flake = m_sim[static_cast<std::size_t>(i)];
    if (flake.radius <= 0.0F) {
      respawn(flake, /*initialScatter=*/false);
    }

    flake.swayPhase += flake.swaySpeed * deltaSeconds;
    // Wind affects bigger/closer flakes more (more apparent surface, more parallax) -
    // same per-depth weighting the rest of this effect uses elsewhere.
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

    SnowFlake& out = m_flakes[static_cast<std::size_t>(i)];
    out.x = flake.x;
    out.y = flake.y;
    out.radius = flake.radius;
  }
}
