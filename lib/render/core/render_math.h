#pragma once

#include "render/core/mat3.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

inline bool isAxisAligned(const Mat3& transform) {
  constexpr float kAxisAlignedEpsilon = 0.0001F;
  return std::abs(transform.m[1]) <= kAxisAlignedEpsilon && std::abs(transform.m[3]) <= kAxisAlignedEpsilon;
}

// Wayland buffer scales are >= 1; the clamp also guards against a zero scale.
inline float snapToBufferPixel(float value, float scale) {
  const float safeScale = std::max(1.0F, scale);
  return std::round(value * safeScale) / safeScale;
}

inline void hashCombine(std::size_t& seed, std::size_t v) {
  seed ^= v + 0x9E3779B97F4A7C15ULL + (seed << 12) + (seed >> 4);
}
