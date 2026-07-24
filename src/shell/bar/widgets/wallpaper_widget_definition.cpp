#include "shell/bar/widgets/wallpaper_widget_definition.h"

const noctalia::bar::WidgetDefinition<WallpaperWidget::Options>& wallpaperWidgetDefinition() {
  using noctalia::bar::field;
  using Options = WallpaperWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "wallpaper",
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
