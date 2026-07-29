#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/session_widget.h"

[[nodiscard]] const noctalia::bar::WidgetDefinition<SessionWidget::Options>& sessionWidgetDefinition();
