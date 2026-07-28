#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/tray_widget.h"

#include <string_view>

struct TrayWidgetDefinitionContext {
  std::string_view barPosition = "top";
  float inlineEntryGap = Style::spaceXs;
};

[[nodiscard]] const noctalia::bar::WidgetDefinition<TrayWidget::Options, TrayWidgetDefinitionContext>&
trayWidgetDefinition();
