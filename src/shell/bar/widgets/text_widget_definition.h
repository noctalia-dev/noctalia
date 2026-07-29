#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/text_widget.h"

[[nodiscard]] const noctalia::bar::WidgetDefinition<TextWidget::Options>& textWidgetDefinition();
