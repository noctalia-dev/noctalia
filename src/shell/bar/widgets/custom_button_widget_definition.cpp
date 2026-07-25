#include "shell/bar/widgets/custom_button_widget_definition.h"

#include "shell/bar/widgets/glyph_button_definition.h"

const noctalia::bar::WidgetDefinition<CustomButtonWidget::Options>& customButtonWidgetDefinition() {
  using noctalia::bar::field;
  using Options = CustomButtonWidget::Options;

  static const settings::WidgetSettingVisibility scrollEnabled{"enable_scroll", {"true"}};

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "custom_button",
      .fields = noctalia::bar::glyphButtonFields<Options>(
          field<&Options::label>({
              .key = "label",
          }),
          field<&Options::tooltip>({
              .key = "tooltip",
          }),
          field<&Options::command>({
              .key = "command",
          }),
          field<&Options::rightCommand>({
              .key = "right_command",
          }),
          field<&Options::middleCommand>({
              .key = "middle_command",
          }),
          field<&Options::enableScroll>({
              .key = "enable_scroll",
          }),
          field<&Options::scrollUpCommand>({
              .key = "scroll_up_command",
              .presentation =
                  settings::WidgetSettingPresentation{
                      .visibleWhen = scrollEnabled,
                  },
          }),
          field<&Options::scrollDownCommand>({
              .key = "scroll_down_command",
              .presentation = settings::WidgetSettingPresentation{
                  .visibleWhen = scrollEnabled,
              },
          })
      ),
  };
  return definition;
}
