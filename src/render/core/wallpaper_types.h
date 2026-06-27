#pragma once

#include "render/core/color.h"
#include "render/core/mat3.h"
#include "render/core/texture_handle.h"

#include <cstdint>

// Config-owned enum; forward-declared here so render/core stays independent of config_types.
enum class WallpaperTransition : std::uint8_t;

enum class WallpaperSourceKind : std::uint8_t {
  Image = 0,
  Color = 1,
};

struct TransitionParams {
  float direction = 0.0f;     // wipe: 0=left, 1=right, 2=up, 3=down
  float centerX = 0.5f;       // disc, honeycomb
  float centerY = 0.5f;       // disc, honeycomb
  float stripeCount = 12.0f;  // stripes
  float angle = 30.0f;        // stripes (degrees)
  float maxBlockSize = 64.0f; // pixelate
  float cellSize = 0.04f;     // honeycomb
  float smoothness = 0.5f;    // wipe, disc, stripes
  float aspectRatio = 1.777f; // disc, stripes, honeycomb (computed at render time)
};

// Geometry for the Span fill mode: a single wallpaper stretched across the whole
// multi-monitor desktop, with each output showing the portion that matches its
// position. All values are in the compositor's logical coordinate space; offset is
// this output's top-left relative to the desktop bounding-box origin. A zero total
// size means span geometry is unavailable and the shader falls back to Crop.
struct WallpaperSpanParams {
  float offsetX = 0.0f;
  float offsetY = 0.0f;
  float monitorWidth = 0.0f;
  float monitorHeight = 0.0f;
  float totalWidth = 0.0f;
  float totalHeight = 0.0f;

  bool operator==(const WallpaperSpanParams&) const = default;
};

// One wallpaper source: either an image texture or a solid color, plus the
// source image's intrinsic size (used for aspect-correct fill modes).
struct WallpaperLayer {
  WallpaperSourceKind kind = WallpaperSourceKind::Image;
  TextureId texture;
  Color color = rgba(0.0f, 0.0f, 0.0f, 1.0f);
  float imageWidth = 0.0f;
  float imageHeight = 0.0f;
};

// All inputs for a single wallpaper draw: the two cross-faded layers plus
// geometry, transition state, and fill parameters. fillMode stays a float so
// render/core does not depend on the config-owned WallpaperFillMode enum.
struct WallpaperDrawParams {
  WallpaperTransition transition{};
  WallpaperLayer from;
  WallpaperLayer to;
  float surfaceWidth = 0.0f;
  float surfaceHeight = 0.0f;
  float quadWidth = 0.0f;
  float quadHeight = 0.0f;
  float progress = 0.0f;
  float fillMode = 0.0f;
  TransitionParams params{};
  Color fillColor = rgba(0.0f, 0.0f, 0.0f, 1.0f);
  Mat3 transform = Mat3::identity();
  WallpaperSpanParams span{};
};
