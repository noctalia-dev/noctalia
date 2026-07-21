#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/brightness_widget.h"

[[nodiscard]] const noctalia::bar::WidgetDefinition<BrightnessWidget::Options>& brightnessWidgetDefinition();
