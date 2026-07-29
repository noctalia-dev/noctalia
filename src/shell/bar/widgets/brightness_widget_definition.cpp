#include "shell/bar/widgets/brightness_widget_definition.h"

const noctalia::bar::WidgetDefinition<BrightnessWidget::Options>& brightnessWidgetDefinition() {
  using noctalia::bar::field;
  using Options = BrightnessWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "brightness",
      .fields = {
          field<&Options::showLabel>({
              .key = "show_label",
          }),
      },
  };
  return definition;
}
