#include "shell/bar/widgets/launcher_widget_definition.h"

const noctalia::bar::WidgetDefinition<LauncherWidget::Options>& launcherWidgetDefinition() {
  using noctalia::bar::field;
  using Options = LauncherWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "launcher",
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
