#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/weather_widget.h"

[[nodiscard]] const noctalia::bar::WidgetDefinition<WeatherWidget::Options>& weatherWidgetDefinition();
