#pragma once

#include "config/config_types.h"
#include "ui/controls/color_swatch_preview.h"

namespace noctalia::theme {
  struct AvailablePalette;
  struct BuiltinPalette;
} // namespace noctalia::theme

// Swatch strips shown next to palette names in pickers. Light mode previews the light variant; every other mode,
// including Auto, previews the dark one.
[[nodiscard]] ColorSwatchPreview
builtinPaletteSwatchPreview(const noctalia::theme::BuiltinPalette& entry, ThemeMode mode);
// Community and custom palettes carry preview metadata; a palette without a light preview falls back to its dark one.
[[nodiscard]] ColorSwatchPreview
availablePaletteSwatchPreview(const noctalia::theme::AvailablePalette& entry, ThemeMode mode);
