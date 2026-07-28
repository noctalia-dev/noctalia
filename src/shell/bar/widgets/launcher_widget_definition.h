#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/launcher_widget.h"

[[nodiscard]] const noctalia::bar::WidgetDefinition<LauncherWidget::Options>& launcherWidgetDefinition();
