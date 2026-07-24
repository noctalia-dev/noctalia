#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/custom_button_widget.h"

[[nodiscard]] const noctalia::bar::WidgetDefinition<CustomButtonWidget::Options>& customButtonWidgetDefinition();
