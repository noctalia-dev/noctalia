#pragma once

#include "theme/fixed_palette.h"

namespace noctalia::theme {

  // Re-anchors the dark surface ramp to true black: every surface-family token is
  // lowered by the tone of `surface`, so the background lands on tone 0 and the
  // elevation ladder keeps its relative steps, hue and chroma. Text, accents and
  // outlines are left alone, so contrast against the surfaces is preserved.
  //
  // Only the dark token map is touched — banding on OLED panels is a dark-background
  // problem, and a light palette has no true-black base to anchor to.
  void applyPureBlackDark(TokenMap& darkTokens);
  void applyPureBlackDark(GeneratedPalette& palette);

  void applyHighContrast(TokenMap& tokens, bool isDark);
  void applyHighContrast(GeneratedPalette& palette);

} // namespace noctalia::theme
