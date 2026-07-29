#include "shell/bar/widgets/screenshot_widget_definition.h"

#include "shell/bar/widgets/glyph_button_definition.h"

const noctalia::bar::WidgetDefinition<ScreenshotWidget::Options>& screenshotWidgetDefinition() {
  using Options = ScreenshotWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "screenshot",
      .fields = noctalia::bar::glyphButtonFields<Options>(),
  };
  return definition;
}
