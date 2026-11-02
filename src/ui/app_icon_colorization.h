#pragma once

#include "render/core/color.h"
#include "ui/palette.h"
#include "ui/signal.h"

#include <cstdint>
#include <optional>

struct ShellConfig;

struct AppIconColorizationStyle {
  Color tint = rgba(1.0F, 1.0F, 1.0F, 1.0F);
  bool monochrome = false;
};

struct ShellAppIconColorizationSettings {
  bool enabled = false;
  std::optional<ColorSpec> color;

  bool operator==(const ShellAppIconColorizationSettings&) const = default;
};

// App-icon bake: desktop/tray/taskbar/dock/launcher-panel bitmap icons (CPU desaturate + tint).

ShellAppIconColorizationSettings shellAppIconColorizationSettings(const ShellConfig& shell) noexcept;
std::optional<ColorSpec> effectiveShellAppIconColorizationTint(const ShellConfig& shell) noexcept;
AppIconColorizationStyle resolveAppIconColorization(const ColorSpec& tint) noexcept;
void bakeAppIconForColorization(std::uint8_t* rgba, int width, int height, const Color& tint) noexcept;

Signal<>& shellAppIconColorizationChanged() noexcept;
void notifyShellAppIconColorizationChanged() noexcept;
