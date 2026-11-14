#include "shell/bar/widgets/battery_widget_definition.h"

#include "shell/bar/glyph_value_definition.h"
#include "system/battery_warning_monitor.h"

const noctalia::bar::WidgetDefinition<BatteryWidget::Options, BatteryWidgetDefinitionContext>&
batteryWidgetDefinition() {
  using noctalia::bar::field;
  using Options = BatteryWidget::Options;
  using noctalia::bar::glyphPositionField;
  using noctalia::bar::peekDelayField;
  using noctalia::bar::peekField;

  settings::WidgetSettingVisibility glyphAndLabel;
  glyphAndLabel.all = {
      {.key = "display_mode", .values = {"glyph"}},
      {.key = "show_label", .values = {"true"}},
  };

  static const noctalia::bar::WidgetDefinition<Options, BatteryWidgetDefinitionContext> definition{
      .type = "battery",
      .fields =
          {
              field<&Options::displayMode>({
                  .key = "display_mode",
                  .choices =
                      {
                          {
                              .value = BatteryDisplayMode::None,
                              .configValue = "none",
                              .labelKey = "settings.widgets.options.none",
                          },
                          {
                              .value = BatteryDisplayMode::Glyph,
                              .configValue = "glyph",
                              .labelKey = "settings.widgets.options.glyph",
                          },
                          {
                              .value = BatteryDisplayMode::Graphic,
                              .configValue = "graphic",
                              .labelKey = "settings.widgets.options.graphic",
                          },
                      },
              }),
              field<&Options::showLabel>({
                  .key = "show_label",
              }),
              field<&Options::labelContent>({
                  .key = "label_content",
                  .choices =
                      {
                          {
                              .value = BatteryLabelContent::Percent,
                              .configValue = "percent",
                              .labelKey = "settings.widgets.options.percent",
                          },
                          {
                              .value = BatteryLabelContent::Time,
                              .configValue = "time",
                              .labelKey = "settings.widgets.options.time",
                          },
                          {
                              .value = BatteryLabelContent::Rate,
                              .configValue = "rate",
                              .labelKey = "settings.widgets.options.rate",
                          },
                      },
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .visibleWhen = settings::WidgetSettingVisibility{"show_label", {"true"}},
                          .horizontalBarOnly = true,
                      },
              }),
              glyphPositionField<Options>(glyphAndLabel),
              peekField<Options>(glyphAndLabel),
              peekDelayField<Options>(glyphAndLabel),
              field<&Options::hideWhenPlugged>({
                  .key = "hide_when_plugged",
              }),
              field<&Options::hideWhenFull>({
                  .key = "hide_when_full",
              }),
              field<&Options::deviceSelector>({
                  .key = "device",
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .options = {{.value = "auto", .labelKey = "common.states.auto"}},
                          .optionSource = settings::WidgetSettingOptionSource::BatteryDevices,
                      },
              }),
              field<&Options::warningColor>({
                  .key = "warning_color",
              }),
          },
      .finalize = [](Options& options, const BatteryWidgetDefinitionContext& context) {
        options.warningThreshold = context.batteryConfig != nullptr
            ? batteryWarningThresholdForSelector(*context.batteryConfig, context.upower, options.deviceSelector)
            : 0;
      },
  };
  return definition;
}
