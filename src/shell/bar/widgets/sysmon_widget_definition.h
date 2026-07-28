#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/sysmon_widget.h"

struct SysmonWidgetDefinitionContext {
  bool verticalBar = false;
};

[[nodiscard]] const noctalia::bar::WidgetDefinition<SysmonWidget::Options, SysmonWidgetDefinitionContext>&
sysmonWidgetDefinition();
