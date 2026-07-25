#include "shell/bar/widgets/screenshot_widget_definition.h"

#include "shell/bar/widgets/glyph_button_definition.h"

const noctalia::bar::WidgetDefinition<ScreenshotWidget::Options>& screenshotWidgetDefinition() {
  using noctalia::bar::field;
  using Options = ScreenshotWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "screenshot",
      .fields = noctalia::bar::glyphButtonFields<Options>(field<&Options::primaryClick>({
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
      })),
  };
  return definition;
}
