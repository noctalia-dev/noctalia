#include "shell/bar/widgets/clock_widget_definition.h"

const noctalia::bar::WidgetDefinition<ClockWidget::Options>& clockWidgetDefinition() {
  using noctalia::bar::field;
  using Options = ClockWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "clock",
      .fields = {
          field<&Options::format>({
              .key = "format",
          }),
          field<&Options::verticalFormat>({
              .key = "vertical_format",
          }),
          field<&Options::tooltipFormat>({
              .key = "tooltip_format",
          }),
          field<&Options::timezone>({
              .key = "timezone",
          }),
      },
  };
  return definition;
}
