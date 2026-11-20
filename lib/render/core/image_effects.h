#pragma once

#include "render/core/color.h"

#include <cstdint>
#include <vector>

// CPU pixel effects applied to decoded RGBA8 (non-premultiplied) images. Pure functions of their inputs, so the
// async texture cache can run them on its decode workers.
namespace image_effects {

  // Three-pass box blur approximating a Gaussian of the given radius in pixels.
  void blur(std::vector<std::uint8_t>& rgba, int width, int height, int radius);

  // App-icon colorization: desaturate, stretch contrast, and tint. `lightTheme` selects the display-level floor.
  void colorizeAppIcon(std::uint8_t* rgba, int width, int height, const Color& tint, bool lightTheme) noexcept;

  // Symbolic icon recolor: alpha-mask icons keep their alpha; opaque monochrome rasters turn luminance into alpha.
  void symbolicMask(std::vector<std::uint8_t>& rgba, const Color& color) noexcept;

} // namespace image_effects
