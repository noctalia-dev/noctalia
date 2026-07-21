#include "shell/settings/settings_popup_layout.h"

#include "ui/popup_chrome.h"
#include "ui/style.h"

#include <algorithm>

namespace settings::popup_layout {

  float preferredContentWidth(
      std::uint32_t parentWidth, float scale, float minLogical, float maxLogical, float parentFraction,
      float parentMarginLogical, const ShellConfig::ShadowConfig& shadow
  ) {
    float panelW = minLogical * scale;
    if (parentWidth == 0) {
      return panelW;
    }

    const auto probe = popup_chrome::computeGeometry(panelW, panelW, shadow, Style::popupShadowsEnabled());
    const float chromeW = static_cast<float>(probe.surfaceWidth) - panelW;
    const float fitPanelW = std::max(1.0f, static_cast<float>(parentWidth) - (parentMarginLogical * scale) - chromeW);
    const float maxPanelW = std::min(fitPanelW, maxLogical * scale);
    const float minPanelW = minLogical * scale;
    const float preferredW = parentFraction * static_cast<float>(parentWidth);
    return std::min(std::max(preferredW, minPanelW), maxPanelW);
  }

  std::pair<std::uint32_t, std::uint32_t>
  surfaceSizeForContent(float contentWidth, float contentHeight, const ShellConfig::ShadowConfig& shadow) {
    const auto geo = popup_chrome::computeGeometry(
        std::max(1.0f, contentWidth), std::max(1.0f, contentHeight), shadow, Style::popupShadowsEnabled()
    );
    return {geo.surfaceWidth, geo.surfaceHeight};
  }

} // namespace settings::popup_layout
