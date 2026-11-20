#include "ui/app_icon_colorization.h"

#include "ui/palette.h"

ColorSpec defaultAppIconColorizationTint() noexcept {
  return colorSpecFromRole(isResolvedLightTheme() ? ColorRole::OnSurface : ColorRole::OnSurfaceVariant);
}

AppIconColorizationStyle resolveAppIconColorization(const ColorSpec& tint) noexcept {
  return AppIconColorizationStyle{.tint = resolveColorSpec(tint), .monochrome = false};
}
