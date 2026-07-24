#include "shell/bar/widgets/control_center_widget_definition.h"

const noctalia::bar::WidgetDefinition<ControlCenterWidget::Options>& controlCenterWidgetDefinition() {
  using noctalia::bar::field;
  using Options = ControlCenterWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "control-center",
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
