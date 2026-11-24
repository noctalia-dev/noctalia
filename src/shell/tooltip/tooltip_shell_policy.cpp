#include "ui/tooltip/tooltip_manager.h"

#include "render/scene/input_area.h"

void TooltipManager::onBarHoverChange(InputArea* area, zwlr_layer_surface_v1* parentLayerSurface, wl_output* output) {
  if (!m_suppressedBarTooltipPanels.empty()) {
    dismissPopup();
    return;
  }
  onHoverChange(area, parentLayerSurface, output);
}

void TooltipManager::suppressBarTooltipsForPanel(std::string_view panelId) {
  if (panelId.empty() || !m_suppressedBarTooltipPanels.emplace(panelId).second) {
    return;
  }
  dismissPopup();
}

void TooltipManager::restoreBarTooltipsForPanel(std::string_view panelId) {
  m_suppressedBarTooltipPanels.erase(std::string(panelId));
}

