#include "shell/bar/widgets/clipboard_widget_definition.h"

#include "shell/bar/glyph_button_definition.h"
#include "shell/bar/glyph_value_definition.h"

const noctalia::bar::WidgetDefinition<ClipboardWidget::Options>& clipboardWidgetDefinition() {
  using noctalia::bar::field;
  using noctalia::bar::glyphPositionField;
  using noctalia::bar::peekDelayField;
  using noctalia::bar::peekField;
  using noctalia::bar::whenAllTrue;
  using Options = ClipboardWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "clipboard",
      .fields = noctalia::bar::glyphButtonFields<Options>(
          field<&Options::showLabel>({
              .key = "show_label",
          }),
          field<&Options::maxWidth>({
              .key = "max_length",
              .minValue = 40.0,
              .maxValue = 800.0,
              .step = 1.0,
              .presentation = settings::WidgetSettingPresentation{.visibleWhen = whenAllTrue({"show_label"})},
          }),
          glyphPositionField<Options>(whenAllTrue({"show_label"})), peekField<Options>(whenAllTrue({"show_label"})),
          peekDelayField<Options>(whenAllTrue({"show_label"}))
      ),
      .glyph = [](const Options& options) { return options.glyph; },
  };
  return definition;
}
