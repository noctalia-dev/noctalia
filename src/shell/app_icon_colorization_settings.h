#pragma once

#include "ui/app_icon_colorization.h"
#include "ui/palette.h"
#include "ui/signal.h"

#include <optional>

struct ShellConfig;

struct ShellAppIconColorizationSettings {
  bool launcherEnabled = false;
  std::optional<ColorSpec> color;

  bool operator==(const ShellAppIconColorizationSettings&) const = default;
};

ShellAppIconColorizationSettings shellAppIconColorizationSettings(const ShellConfig& shell) noexcept;
std::optional<ColorSpec> effectiveShellAppIconColorizationTint(const ShellConfig& shell, bool enabled) noexcept;

Signal<>& shellAppIconColorizationChanged() noexcept;
void notifyShellAppIconColorizationChanged() noexcept;
