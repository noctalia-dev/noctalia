#include "shell/bar/widgets/bluetooth_widget_definition.h"

namespace {

  // A powered-off adapter has no connected device, so hiding on "no connected device"
  // already covers the adapter-off case and makes this toggle inert.
  settings::WidgetSettingVisibility connectedDeviceFilterOff() {
    settings::WidgetSettingVisibility visibility;
    visibility.all = {{"hide_when_no_connected_device", {"false"}}};
    return visibility;
  }

} // namespace

const noctalia::bar::WidgetDefinition<BluetoothWidget::Options>& bluetoothWidgetDefinition() {
  using noctalia::bar::field;
  using Options = BluetoothWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "bluetooth",
      .fields = {
          field<&Options::showLabel>({
              .key = "show_label",
          }),
          field<&Options::hideWhenAdapterOff>({
              .key = "hide_when_adapter_off",
              .presentation =
                  settings::WidgetSettingPresentation{
                      .visibleWhen = connectedDeviceFilterOff(),
                  },
          }),
          field<&Options::hideWhenNoConnectedDevice>({
              .key = "hide_when_no_connected_device",
          }),
      },
  };
  return definition;
}
