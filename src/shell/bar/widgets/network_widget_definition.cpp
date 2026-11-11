#include "shell/bar/widgets/network_widget_definition.h"

#include "shell/bar/glyph_value_definition.h"

namespace {

  settings::WidgetSettingVisibility vpnLabelVisibility() {
    settings::WidgetSettingVisibility visibility;
    visibility.all = {
        {"show_label", {"true"}},
        {"vpn_status", {"replace", "both"}},
    };
    return visibility;
  }

} // namespace

const noctalia::bar::WidgetDefinition<NetworkWidget::Options>& networkWidgetDefinition() {
  using noctalia::bar::field;
  using Options = NetworkWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "network",
      .fields = {
          field<&Options::vpnStatusMode>({
              .key = "vpn_status",
              .choices =
                  {
                      {
                          .value = VpnStatusMode::Replace,
                          .configValue = "replace",
                          .labelKey = "settings.widgets.options.replace",
                      },
                      {
                          .value = VpnStatusMode::Both,
                          .configValue = "both",
                          .labelKey = "settings.widgets.options.both",
                      },
                      {
                          .value = VpnStatusMode::Hidden,
                          .configValue = "hidden",
                          .labelKey = "settings.widgets.options.hidden",
                      },
                  },
          }),
          field<&Options::showLabel>({
              .key = "show_label",
          }),
          noctalia::bar::glyphPositionField<Options>(noctalia::bar::whenAllTrue({"show_label"})),
          noctalia::bar::peekField<Options>(noctalia::bar::whenAllTrue({"show_label"})),
          noctalia::bar::peekDelayField<Options>(noctalia::bar::whenAllTrue({"show_label"})),
          field<&Options::showVpnLabel>({
              .key = "show_vpn_label",
              .presentation = settings::WidgetSettingPresentation{
                  .visibleWhen = vpnLabelVisibility(),
              },
          }),
      },
  };
  return definition;
}
