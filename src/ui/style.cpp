#include "ui/style.h"

#include <algorithm>

namespace {

  float g_cornerRadiusScale = 1.0f;
  bool g_buttonBordersEnabled = true;
  bool g_inputBordersEnabled = true;
  bool g_popupBordersEnabled = true;
  bool g_profileBordersEnabled = true;
  bool g_hoverBordersEnabled = true;
  bool g_popupShadowsEnabled = true;

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

  bool inputBordersEnabled() noexcept { return g_inputBordersEnabled; }

  void setInputBordersEnabled(bool enabled) {
    if (g_inputBordersEnabled == enabled) {
      return;
    }
    g_inputBordersEnabled = enabled;
    inputBordersChanged().emit();
  }

  Signal<>& inputBordersChanged() {
    static Signal<> signal;
    return signal;
  }

  bool popupBordersEnabled() noexcept { return g_popupBordersEnabled; }
  void setPopupBordersEnabled(bool enabled) { g_popupBordersEnabled = enabled; }

  bool profileBordersEnabled() noexcept { return g_profileBordersEnabled; }

  void setProfileBordersEnabled(bool enabled) {
    if (g_profileBordersEnabled == enabled) {
      return;
    }
    g_profileBordersEnabled = enabled;
    profileBordersChanged().emit();
  }

  Signal<>& profileBordersChanged() {
    static Signal<> signal;
    return signal;
  }

  bool hoverBordersEnabled() noexcept { return g_hoverBordersEnabled; }

  void setHoverBordersEnabled(bool enabled) {
    if (g_hoverBordersEnabled == enabled) {
      return;
    }
    g_hoverBordersEnabled = enabled;
    hoverBordersChanged().emit();
  }

  Signal<>& hoverBordersChanged() {
    static Signal<> signal;
    return signal;
  }

  bool popupShadowsEnabled() noexcept { return g_popupShadowsEnabled; }
  void setPopupShadowsEnabled(bool enabled) { g_popupShadowsEnabled = enabled; }

  float scaledRadius(float radius, float localScale) noexcept { return radius * localScale * g_cornerRadiusScale; }

  float scaledRadiusSm(float localScale) noexcept { return scaledRadius(radiusSm, localScale); }

  float scaledRadiusMd(float localScale) noexcept { return scaledRadius(radiusMd, localScale); }

  float scaledRadiusLg(float localScale) noexcept { return scaledRadius(radiusLg, localScale); }

  float scaledRadiusXl(float localScale) noexcept { return scaledRadius(radiusXl, localScale); }

} // namespace Style
