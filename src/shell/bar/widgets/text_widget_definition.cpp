#include "shell/bar/widgets/text_widget_definition.h"

const noctalia::bar::WidgetDefinition<TextWidget::Options>& textWidgetDefinition() {
  using noctalia::bar::field;
  using Options = TextWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "text",
      .fields = {
          field<&Options::text>({
              .key = "text",
          }),
      },
  };
  return definition;
}
