#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/clock_widget.h"

[[nodiscard]] const noctalia::bar::WidgetDefinition<ClockWidget::Options>& clockWidgetDefinition();
