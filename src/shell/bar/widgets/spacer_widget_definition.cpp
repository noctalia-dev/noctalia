#include "shell/bar/widgets/spacer_widget_definition.h"

const noctalia::bar::WidgetDefinition<SpacerWidget::Options>& spacerWidgetDefinition() {
  using noctalia::bar::field;
  using Options = SpacerWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "spacer",
      .fields = {
          field<&Options::length>({
              .key = "length",
              .minValue = 0.0,
              .maxValue = 400.0,
              .step = 1.0,
          }),
      },
  };
  return definition;
}
