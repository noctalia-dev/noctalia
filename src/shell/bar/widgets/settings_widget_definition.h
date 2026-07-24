#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/settings_widget.h"

[[nodiscard]] const noctalia::bar::WidgetDefinition<SettingsWidget::Options>& settingsWidgetDefinition();
