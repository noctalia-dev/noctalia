#include "shell/bar/widgets/clipboard_widget_definition.h"

#include "shell/bar/widgets/glyph_button_definition.h"

const noctalia::bar::WidgetDefinition<ClipboardWidget::Options>& clipboardWidgetDefinition() {
  using Options = ClipboardWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "clipboard",
      .fields = noctalia::bar::glyphButtonFields<Options>(),
      .glyph = [](const Options& options) { return options.glyph; },
  };
  return definition;
}
