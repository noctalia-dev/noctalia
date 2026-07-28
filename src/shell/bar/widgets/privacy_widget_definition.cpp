#include "shell/bar/widgets/privacy_widget_definition.h"

const noctalia::bar::WidgetDefinition<PrivacyWidget::Options>& privacyWidgetDefinition() {
  using noctalia::bar::field;
  using Options = PrivacyWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "privacy",
      .fields = {
          field<&Options::hideInactive>({
              .key = "hide_inactive",
          }),
          field<&Options::iconSpacing>({
              .key = "icon_spacing",
              .minValue = 0.0,
              .maxValue = 48.0,
              .step = 1.0,
          }),
          field<&Options::activeColor>({
              .key = "active_color",
          }),
          field<&Options::inactiveColor>({
              .key = "inactive_color",
          }),
      },
  };
  return definition;
}
