#include "shell/bar/widgets/custom_button_widget_definition.h"

#include "shell/bar/widgets/glyph_button_definition.h"

const noctalia::bar::WidgetDefinition<CustomButtonWidget::Options>& customButtonWidgetDefinition() {
  using noctalia::bar::field;
  using Options = CustomButtonWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "custom_button",
      .fields = noctalia::bar::glyphButtonFields<Options>(
          field<&Options::label>({
              .key = "label",
          }),
          field<&Options::tooltip>({
              .key = "tooltip",
          })
      ),
  };
  return definition;
}
