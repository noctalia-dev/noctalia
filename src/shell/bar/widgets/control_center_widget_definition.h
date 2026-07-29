#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/control_center_widget.h"

[[nodiscard]] const noctalia::bar::WidgetDefinition<ControlCenterWidget::Options>& controlCenterWidgetDefinition();
