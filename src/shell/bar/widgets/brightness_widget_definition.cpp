#include "shell/bar/widgets/brightness_widget_definition.h"

const noctalia::bar::WidgetDefinition<BrightnessWidget::Options>& brightnessWidgetDefinition() {
  using noctalia::bar::field;
  using Options = BrightnessWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "brightness",
      .fields =
          {
              field<&Options::showGlyph>({
                  .key = "show_glyph",
              }),
              field<&Options::showLabel>({
                  .key = "show_label",
              }),
              field<&Options::showWhenUnavailable>({
                  .key = "show_when_unavailable",
              }),
          },
      .validateOptions = [](const Options& options) -> std::optional<std::string> {
        if (!options.showGlyph && !options.showLabel) {
          return "show_glyph and show_label cannot both be false";
        }
        return std::nullopt;
      },
  };
  return definition;
}
