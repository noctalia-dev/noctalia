#include "shell/bar/widgets/settings_widget_definition.h"

const noctalia::bar::WidgetDefinition<SettingsWidget::Options>& settingsWidgetDefinition() {
  using noctalia::bar::field;
  using Options = SettingsWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "settings",
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
