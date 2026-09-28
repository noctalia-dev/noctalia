#pragma once

#include "render/core/mat3.h"
#include "render/core/texture_handle.h"

#include <cstdint>

enum class LockscreenTransitionKind : std::uint8_t {
  Fade = 0,
  Wipe = 1,
  Disc = 2,
  Stripes = 3,
  Zoom = 4,
  Honeycomb = 5,
};

struct LockscreenTransitionParams {
  float direction = 0.0F;
  float centerX = 0.5F;
  float centerY = 0.5F;
  float stripeCount = 12.0F;
  float angle = 30.0F;
  float cellSize = 0.04F;
  float smoothness = 0.3F;
  float aspectRatio = 1.777F;
};

struct LockscreenTransitionDrawParams {
  LockscreenTransitionKind transition = LockscreenTransitionKind::Fade;
  TextureId texture;
  float surfaceWidth = 0.0F;
  float surfaceHeight = 0.0F;
  float quadWidth = 0.0F;
  float quadHeight = 0.0F;
  float progress = 0.0F;
  float opacity = 1.0F;
  LockscreenTransitionParams params{};
  Mat3 transform = Mat3::identity();
};
