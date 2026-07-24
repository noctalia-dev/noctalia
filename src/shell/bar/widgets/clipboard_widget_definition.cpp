#include "shell/bar/widgets/clipboard_widget_definition.h"

const noctalia::bar::WidgetDefinition<ClipboardWidget::Options>& clipboardWidgetDefinition() {
  using noctalia::bar::field;
  using Options = ClipboardWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "clipboard",
      .fields = {
          field<&Options::glyph>({
              .key = "glyph",
              .control = settings::WidgetControlKind::Glyph,
          }),
          field<&Options::customImage>({
              .key = "custom_image",
          }),
          field<&Options::customImageColorize>({
              .key = "custom_image_colorize",
          }),
      },
  };
  return definition;
}
