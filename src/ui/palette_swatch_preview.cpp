#include "ui/palette_swatch_preview.h"

#include "render/core/color.h"
#include "theme/builtin_palettes.h"
#include "theme/community_palettes.h"

namespace {

  ColorSwatchPreview previewFromFixedPalette(const noctalia::theme::FixedPalette& colors) {
    return ColorSwatchPreview{
        .surface = fixedColorSpec(colors.surface),
        .swatches = {
            fixedColorSpec(colors.primary),
            fixedColorSpec(colors.secondary),
            fixedColorSpec(colors.tertiary),
            fixedColorSpec(colors.error),
        },
    };
  }

  ColorSwatchPreview previewFromMetadata(const noctalia::theme::AvailablePalette::PreviewMode& metadata) {
    ColorSwatchPreview preview;
    Color surface;
    if (tryParseHexColor(metadata.surface, surface)) {
      preview.surface = fixedColorSpec(surface);
    }
    preview.swatches.reserve(metadata.accents.size());
    for (const auto& hexColor : metadata.accents) {
      Color color;
      if (tryParseHexColor(hexColor, color)) {
        preview.swatches.push_back(fixedColorSpec(color));
      }
    }
    return preview;
  }

} // namespace

ColorSwatchPreview builtinPaletteSwatchPreview(const noctalia::theme::BuiltinPalette& entry, ThemeMode mode) {
  return previewFromFixedPalette(mode == ThemeMode::Light ? entry.light.palette : entry.dark.palette);
}

ColorSwatchPreview availablePaletteSwatchPreview(const noctalia::theme::AvailablePalette& entry, ThemeMode mode) {
  if (mode == ThemeMode::Light) {
    ColorSwatchPreview preview = previewFromMetadata(entry.preview.light);
    if (!preview.empty()) {
      return preview;
    }
  }
  return previewFromMetadata(entry.preview.dark);
}
