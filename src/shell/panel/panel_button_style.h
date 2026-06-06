#pragma once

#include "ui/controls/button.h"
#include "ui/style.h"

namespace panel_button_style {

  inline void configureHeaderIconButton(Button& button, float scale) {
    button.setVariant(ButtonVariant::Default);
    button.setGlyphSize(Style::fontSizeBody * scale);
    button.setMinWidth(Style::controlHeightSm * scale);
    button.setMinHeight(Style::controlHeightSm * scale);
    button.setPadding(Style::spaceXs * scale);
    button.setRadius(Style::scaledRadiusMd(scale));
  }

} // namespace panel_button_style
