#include "shell/bar/widgets/power_profile_widget_definition.h"

const noctalia::bar::WidgetDefinition<PowerProfileWidget::Options>& powerProfileWidgetDefinition() {
  using noctalia::bar::field;
  using Options = PowerProfileWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "power_profile",
      .fields = {
          field<&Options::enableScroll>({
              .key = "enable_scroll",
          }),
      },
  };
  return definition;
}
