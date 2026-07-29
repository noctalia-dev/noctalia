#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/clipboard_widget.h"

[[nodiscard]] const noctalia::bar::WidgetDefinition<ClipboardWidget::Options>& clipboardWidgetDefinition();
