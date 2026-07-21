#include "shell/bar/widgets/brightness_widget_definition.h"

const noctalia::bar::WidgetDefinition<BrightnessWidget::Options>& brightnessWidgetDefinition() {
  using noctalia::bar::field;
  using Options = BrightnessWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "brightness",
      .fields = {
          field<&Options::enableScroll>({
              .key = "enable_scroll",
          }),
          field<&Options::scrollStepPercent>({
              .key = "scroll_step",
              .minValue = 1.0,
              .maxValue = 25.0,
              .step = 1.0,
              .presentation =
                  settings::WidgetSettingPresentation{
                      .stepper = true,
                      .valueSuffix = "%",
                      .visibleWhen = settings::WidgetSettingVisibility{"enable_scroll", {"true"}},
                  },
          }),
          field<&Options::showLabel>({
              .key = "show_label",
          }),
      },
  };
  return definition;
}
