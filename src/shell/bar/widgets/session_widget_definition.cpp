#include "shell/bar/widgets/session_widget_definition.h"

const noctalia::bar::WidgetDefinition<SessionWidget::Options>& sessionWidgetDefinition() {
  using noctalia::bar::field;
  using Options = SessionWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "session",
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
