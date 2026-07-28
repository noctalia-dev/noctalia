#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/bluetooth_widget.h"

[[nodiscard]] const noctalia::bar::WidgetDefinition<BluetoothWidget::Options>& bluetoothWidgetDefinition();
