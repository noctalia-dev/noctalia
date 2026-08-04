#pragma once

#include "ui/controls/image.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

class Renderer;

struct WidgetCustomImage {
  std::string path;
  bool colorize = false;

  [[nodiscard]] bool enabled() const noexcept { return !path.empty(); }
};

namespace widget_custom_image {

  // Builds the runtime value from the `custom_image` / `custom_image_colorize` widget settings,
  // expanding a leading `~` in the configured path.
  [[nodiscard]] WidgetCustomImage fromConfig(const std::string& path, bool colorize);

  [[nodiscard]] inline float logicalSize(float contentScale) noexcept { return Style::baseGlyphSize * contentScale; }

  [[nodiscard]] inline int targetSize(float contentScale) noexcept {
    return std::max(1, static_cast<int>(std::round(48.0F * contentScale)));
  }

  inline void syncTint(Image& image, const WidgetCustomImage& customImage, ColorSpec tint) {
    image.setForegroundTint(customImage.colorize ? std::optional<ColorSpec>{tint} : std::nullopt);
  }

  inline void
  sync(Image& image, Renderer& renderer, const WidgetCustomImage& customImage, float contentScale, ColorSpec tint) {
    syncTint(image, customImage, tint);
    const float size = logicalSize(contentScale);
    image.setSize(size, size);
    image.setSourceFile(renderer, customImage.path, targetSize(contentScale), true);
  }

} // namespace widget_custom_image
