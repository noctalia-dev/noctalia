#pragma once

#include "ui/signal.h"

namespace Style {

  inline constexpr int barThicknessDefault = 34;

  inline constexpr int animFast = 100;
  inline constexpr int animNormal = 200;
  inline constexpr int animSlow = 400;

  inline constexpr float radiusSm = 3.0F;
  inline constexpr float radiusMd = 6.0F;
  inline constexpr float radiusLg = 9.0F;
  inline constexpr float radiusXl = 12.0F;

  inline constexpr float borderWidth = 1.0F;
  inline constexpr float emphasizedBorderWidth = 3.0F;
  inline constexpr float focusRingWidth = 2.0F;
  inline constexpr float disabledOutlineAlpha = 0.5F;

  inline constexpr float spaceXs = 4.0F;
  inline constexpr float spaceSm = 8.0F;
  inline constexpr float spaceMd = 12.0F;
  inline constexpr float spaceLg = 16.0F;

  inline constexpr float cardPadding = 14.0F;
  inline constexpr float panelPadding = 14.0F;

  // Default inner inset for bar widget capsules (logical px, before bar content scale).
  inline constexpr float barCapsulePadding = 6.0F;
  inline constexpr float baseGlyphSize = 16.0F;

  inline constexpr float fontSizeMini = 11.0F;
  inline constexpr float fontSizeCaption = 13.0F;
  inline constexpr float fontSizeBody = 14.0F;
  inline constexpr float fontSizeTitle = 16.0F;
  inline constexpr float fontSizeHeader = 20.0F;

  inline constexpr float controlHeightSm = 32.0F;
  inline constexpr float controlHeight = 38.0F;
  inline constexpr float controlHeightLg = 44.0F;
  inline constexpr float scrollWheelStep = 56.0F;
  // Pointer distance in logical px before an armed drag becomes active.
  inline constexpr float dragStartThreshold = 6.0F;

  inline constexpr float scrollbarWidth = 6.0F;
  inline constexpr float scrollbarGap = spaceSm;
  inline constexpr float scrollbarMinThumbHeight = 24.0F;

  // Growth cap (logical px, before content scale) for menus/dropdowns that size to their content.
  inline constexpr float menuAutoMaxWidth = 420.0F;

  // Toggle preset geometry. Track height = thumb + 2 * inset; track width = thumb + 2 * inset + travel.
  inline constexpr float toggleThumbSizeSm = 14.0F;
  inline constexpr float toggleInsetSm = 2.0F;
  inline constexpr float toggleTravelSm = 12.0F;
  inline constexpr float toggleThumbSizeMd = 18.0F;
  inline constexpr float toggleInsetMd = 3.0F;
  inline constexpr float toggleTravelMd = 16.0F;
  inline constexpr float toggleThumbSizeLg = 22.0F;
  inline constexpr float toggleInsetLg = 4.0F;
  inline constexpr float toggleTravelLg = 20.0F;

  // Slider geometry.
  inline constexpr float sliderDefaultWidth = 180.0F;
  inline constexpr float sliderTrackHeight = 8.0F;
  inline constexpr float sliderThumbSize = 18.0F;
  inline constexpr float sliderHorizontalPadding = 2.0F;

  [[nodiscard]] float cornerRadiusScale() noexcept;
  void setCornerRadiusScale(float scale) noexcept;

  [[nodiscard]] bool buttonBordersEnabled() noexcept;
  void setButtonBordersEnabled(bool enabled);
  Signal<>& buttonBordersChanged();

  [[nodiscard]] bool inputBordersEnabled() noexcept;
  void setInputBordersEnabled(bool enabled);
  Signal<>& inputBordersChanged();

  [[nodiscard]] bool popupBordersEnabled() noexcept;
  void setPopupBordersEnabled(bool enabled);

  [[nodiscard]] bool cardBordersEnabled() noexcept;
  void setCardBordersEnabled(bool enabled);

  [[nodiscard]] bool popupShadowsEnabled() noexcept;
  void setPopupShadowsEnabled(bool enabled);

  [[nodiscard]] float scaledRadius(float radius, float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusSm(float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusMd(float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusLg(float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusXl(float localScale = 1.0F) noexcept;

} // namespace Style
