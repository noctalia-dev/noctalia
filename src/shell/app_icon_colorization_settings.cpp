#include "shell/app_icon_colorization_settings.h"

#include "config/config_types.h"

ShellAppIconColorizationSettings shellAppIconColorizationSettings(const ShellConfig& shell) noexcept {
  return ShellAppIconColorizationSettings{.launcherEnabled = shell.launcher.colorizeIcons, .color = shell.appIconColor};
}

Signal<>& shellAppIconColorizationChanged() noexcept {
  static Signal<> signal;
  return signal;
}

void notifyShellAppIconColorizationChanged() noexcept { shellAppIconColorizationChanged().emit(); }

std::optional<ColorSpec> effectiveShellAppIconColorizationTint(const ShellConfig& shell, bool enabled) noexcept {
  if (!enabled) {
    return std::nullopt;
  }
  if (shell.appIconColor.has_value()) {
    return shell.appIconColor;
  }
  return defaultAppIconColorizationTint();
}
