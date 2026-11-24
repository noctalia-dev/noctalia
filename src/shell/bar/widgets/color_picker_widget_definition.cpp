#include "shell/bar/widgets/color_picker_widget_definition.h"

#include "shell/bar/glyph_button_definition.h"

const noctalia::bar::WidgetDefinition<ColorPickerWidget::Options>& colorPickerWidgetDefinition() {
  using Options = ColorPickerWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "color_picker",
      .fields = noctalia::bar::glyphButtonFields<Options>(),
      .glyph = [](const Options& options) { return options.glyph; },
  };
  return definition;
}
