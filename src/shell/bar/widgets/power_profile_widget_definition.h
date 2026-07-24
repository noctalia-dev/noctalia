#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/power_profile_widget.h"

[[nodiscard]] const noctalia::bar::WidgetDefinition<PowerProfileWidget::Options>& powerProfileWidgetDefinition();
