#include "ui/style.h"

#include <algorithm>

namespace {

  float g_cornerRadiusScale = 1.0f;
  bool g_buttonBordersEnabled = true;
  bool g_inputBordersEnabled = true;
  bool g_popupBordersEnabled = true;
  bool g_popupShadowsEnabled = true;
  bool g_pureBlackContextMenusEnabled = false;

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

  void setPopupBordersEnabled(bool enabled) {
    if (g_popupBordersEnabled == enabled) {
      return;
    }
    g_popupBordersEnabled = enabled;
    popupBordersChanged().emit();
  }

  Signal<>& popupBordersChanged() {
    static Signal<> signal;
    return signal;
  }

  bool popupShadowsEnabled() noexcept { return g_popupShadowsEnabled; }

  void setPopupShadowsEnabled(bool enabled) {
    if (g_popupShadowsEnabled == enabled) {
      return;
    }
    g_popupShadowsEnabled = enabled;
    popupShadowsChanged().emit();
  }

  Signal<>& popupShadowsChanged() {
    static Signal<> signal;
    return signal;
  }

  bool pureBlackContextMenusEnabled() noexcept { return g_pureBlackContextMenusEnabled; }

  void setPureBlackContextMenusEnabled(bool enabled) {
    if (g_pureBlackContextMenusEnabled == enabled) {
      return;
    }
    g_pureBlackContextMenusEnabled = enabled;
    pureBlackContextMenusChanged().emit();
  }

  Signal<>& pureBlackContextMenusChanged() {
    static Signal<> signal;
    return signal;
  }

  float scaledRadius(float radius, float localScale) noexcept { return radius * localScale * g_cornerRadiusScale; }

  float scaledRadiusSm(float localScale) noexcept { return scaledRadius(radiusSm, localScale); }

  float scaledRadiusMd(float localScale) noexcept { return scaledRadius(radiusMd, localScale); }

  float scaledRadiusLg(float localScale) noexcept { return scaledRadius(radiusLg, localScale); }

  float scaledRadiusXl(float localScale) noexcept { return scaledRadius(radiusXl, localScale); }

} // namespace Style
