#include "shell/bar/widgets/screenshot_widget_definition.h"

const noctalia::bar::WidgetDefinition<ScreenshotWidget::Options>& screenshotWidgetDefinition() {
  using noctalia::bar::field;
  using Options = ScreenshotWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "screenshot",
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
          field<&Options::primaryClick>({
              .key = "primary_click",
              .choices =
                  {
                      {
                          .value = ScreenshotWidget::PrimaryClick::Region,
                          .configValue = "region",
                          .labelKey = "settings.widgets.options.screenshot-primary-region",
                      },
                      {
                          .value = ScreenshotWidget::PrimaryClick::Fullscreen,
                          .configValue = "fullscreen",
                          .labelKey = "settings.widgets.options.screenshot-primary-fullscreen",
                      },
                  },
              .presentation = settings::WidgetSettingPresentation{
                  .segmented = true,
              },
          }),
      },
  };
  return definition;
}
