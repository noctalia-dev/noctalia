#include "ui/style.h"

#include <algorithm>

namespace {

  float g_cornerRadiusScale = 1.0f;
  bool g_buttonBordersEnabled = true;
  bool g_pureBlackDarkEnabled = false;

} // namespace

namespace Style {

  float cornerRadiusScale() noexcept { return g_cornerRadiusScale; }

  void setCornerRadiusScale(float scale) noexcept { g_cornerRadiusScale = std::clamp(scale, 0.0f, 2.0f); }

  bool buttonBordersEnabled() noexcept { return g_buttonBordersEnabled; }

  void setButtonBordersEnabled(bool enabled) {
    if (g_buttonBordersEnabled == enabled) {
      return;
    }
    g_buttonBordersEnabled = enabled;
    buttonBordersChanged().emit();
  }

  Signal<>& buttonBordersChanged() {
    static Signal<> signal;
    return signal;
  }

  bool pureBlackDarkEnabled() noexcept { return g_pureBlackDarkEnabled; }

  void setPureBlackDarkEnabled(bool enabled) { g_pureBlackDarkEnabled = enabled; }

  float scaledRadius(float radius, float localScale) noexcept { return radius * localScale * g_cornerRadiusScale; }

  float scaledRadiusSm(float localScale) noexcept { return scaledRadius(radiusSm, localScale); }

  float scaledRadiusMd(float localScale) noexcept { return scaledRadius(radiusMd, localScale); }

  float scaledRadiusLg(float localScale) noexcept { return scaledRadius(radiusLg, localScale); }

  float scaledRadiusXl(float localScale) noexcept { return scaledRadius(radiusXl, localScale); }

} // namespace Style
