#pragma once

#include "render/core/color.h"
#include "ui/palette.h"
#include "ui/signal.h"

#include <cstdint>
#include <optional>

struct ShellConfig;

struct AppIconColorizationStyle {
  Color tint = rgba(1.0f, 1.0f, 1.0f, 1.0f);
  bool monochrome = false;
};

struct ShellAppIconColorizationSettings {
  bool enabled = false;
  std::optional<ColorSpec> color;

  bool operator==(const ShellAppIconColorizationSettings&) const = default;
};

// App-icon bake: desktop/tray/taskbar/dock/launcher-panel bitmap icons (CPU desaturate + tint).

[[nodiscard]] ShellAppIconColorizationSettings shellAppIconColorizationSettings(const ShellConfig& shell) noexcept;
[[nodiscard]] std::optional<ColorSpec> effectiveShellAppIconColorizationTint(const ShellConfig& shell) noexcept;
[[nodiscard]] AppIconColorizationStyle resolveAppIconColorization(const ColorSpec& tint) noexcept;
void bakeAppIconForColorization(std::uint8_t* rgba, int width, int height, const Color& tint) noexcept;

[[nodiscard]] Signal<>& shellAppIconColorizationChanged() noexcept;
void notifyShellAppIconColorizationChanged() noexcept;
