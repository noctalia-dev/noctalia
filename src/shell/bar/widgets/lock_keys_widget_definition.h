#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/lock_keys_widget.h"

[[nodiscard]] const noctalia::bar::WidgetDefinition<LockKeysWidget::Options>& lockKeysWidgetDefinition();
