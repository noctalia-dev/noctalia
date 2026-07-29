#pragma once

#include "theme/color.h"

namespace noctalia::theme {

  double relativeLuminance(int r, int g, int b);
  double contrastRatio(const Color& a, const Color& b);
  bool isDark(const Color& c);

  // Binary-search the foreground's OKLCH lightness toward black or white until
  // its sRGB WCAG contrast ratio against `background` meets `minRatio`. Chroma
  // is reduced only when needed to keep the adjusted color inside the sRGB gamut.
  Color ensureContrast(const Color& foreground, const Color& background, double minRatio = 4.5, int preferLight = 0);

} // namespace noctalia::theme
