#pragma once

#include "render/core/color.h"
#include "ui/palette.h"

#include <cstdint>

struct AppIconColorizationStyle {
  Color tint = rgba(1.0F, 1.0F, 1.0F, 1.0F);
  bool monochrome = false;
};

// App-icon bake: desktop/tray/taskbar/dock/launcher-panel bitmap icons (CPU desaturate + tint).

// Theme-dependent tint used when no explicit icon color is configured.
ColorSpec defaultAppIconColorizationTint() noexcept;
AppIconColorizationStyle resolveAppIconColorization(const ColorSpec& tint) noexcept;
void bakeAppIconForColorization(std::uint8_t* rgba, int width, int height, const Color& tint) noexcept;
