#include "shell/bar/widgets/bluetooth_widget_definition.h"

const noctalia::bar::WidgetDefinition<BluetoothWidget::Options>& bluetoothWidgetDefinition() {
  using noctalia::bar::field;
  using Options = BluetoothWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "bluetooth",
      .fields = {
          field<&Options::showLabel>({
              .key = "show_label",
          }),
          field<&Options::hideWhenNoConnectedDevice>({
              .key = "hide_when_no_connected_device",
          }),
      },
  };
  return definition;
}
