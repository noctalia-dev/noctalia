#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/network_widget.h"

[[nodiscard]] const noctalia::bar::WidgetDefinition<NetworkWidget::Options>& networkWidgetDefinition();
