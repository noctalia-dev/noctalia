#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/active_window_widget.h"

[[nodiscard]] const noctalia::bar::WidgetDefinition<ActiveWindowWidget::Options>& activeWindowWidgetDefinition();
