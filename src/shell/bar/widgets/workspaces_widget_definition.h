#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/workspaces_widget.h"

[[nodiscard]] const noctalia::bar::WidgetDefinition<WorkspacesWidget::Options>& workspacesWidgetDefinition();
