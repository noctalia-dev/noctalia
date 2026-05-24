#pragma once

#include "ui/controls/button.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>

namespace panel_button_style {

  inline Button::ButtonPalette translucentHeaderPalette(float fillOpacity) {
    constexpr float kDisabledAlpha = 0.55f;
    const float opacity = std::clamp(fillOpacity, 0.0f, 1.0f);
    return Button::ButtonPalette{
        .borderWidth = Style::borderWidth,
        .normal =
            Button::ButtonStateColors{
                .bg = colorSpecFromRole(ColorRole::SurfaceVariant, opacity),
                .border = colorSpecFromRole(ColorRole::Outline, 0.5f),
                .label = colorSpecFromRole(ColorRole::OnSurface),
            },
        .hover =
            Button::ButtonStateColors{
                .bg = colorSpecFromRole(ColorRole::Hover, std::max(opacity, 0.78f)),
                .border = clearColorSpec(),
                .label = colorSpecFromRole(ColorRole::OnHover),
            },
        .pressed =
            Button::ButtonStateColors{
                .bg = colorSpecFromRole(ColorRole::Primary),
                .border = colorSpecFromRole(ColorRole::Primary),
                .label = colorSpecFromRole(ColorRole::OnPrimary),
            },
        .disabled = Button::ButtonStateColors{
            .bg = colorSpecFromRole(ColorRole::SurfaceVariant, opacity * kDisabledAlpha),
            .border = colorSpecFromRole(ColorRole::Outline, 0.5f * kDisabledAlpha),
            .label = colorSpecFromRole(ColorRole::OnSurface, kDisabledAlpha),
        },
    };
  }

  inline void applyHeaderButtonStyle(Button& button, float fillOpacity) {
    if (fillOpacity >= 0.999f) {
      button.setVariant(ButtonVariant::Default);
      return;
    }

    button.setVariant(ButtonVariant::Outline);
    button.setCustomPalette(translucentHeaderPalette(fillOpacity));
  }

  inline void configureHeaderIconButton(Button& button, float scale, float fillOpacity) {
    applyHeaderButtonStyle(button, fillOpacity);
    button.setGlyphSize(Style::fontSizeBody * scale);
    button.setMinWidth(Style::controlHeightSm * scale);
    button.setMinHeight(Style::controlHeightSm * scale);
    button.setPadding(Style::spaceXs * scale);
    button.setRadius(Style::scaledRadiusMd(scale));
  }

} // namespace panel_button_style
