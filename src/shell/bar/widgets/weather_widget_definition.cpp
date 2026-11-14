#include "shell/bar/widgets/weather_widget_definition.h"

#include "shell/bar/glyph_value_definition.h"

const noctalia::bar::WidgetDefinition<WeatherWidget::Options>& weatherWidgetDefinition() {
  using noctalia::bar::field;
  using noctalia::bar::glyphPositionField;
  using noctalia::bar::peekDelayField;
  using noctalia::bar::peekField;
  using Options = WeatherWidget::Options;

  settings::WidgetSettingVisibility textShown;
  textShown.any = {
      {.key = "show_condition", .values = {"true"}},
      {.key = "show_temperature", .values = {"true"}},
  };

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "weather",
      .fields = {
          field<&Options::maxWidth>({
              .key = "max_length",
              .minValue = 40.0,
              .maxValue = 800.0,
              .step = 1.0,
          }),
          field<&Options::showCondition>({
              .key = "show_condition",
          }),
          field<&Options::showTemperature>({
              .key = "show_temperature",
          }),
          glyphPositionField<Options>(textShown),
          peekField<Options>(textShown),
          peekDelayField<Options>(textShown),
      },
  };
  return definition;
}
