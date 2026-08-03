#include "shell/bar/widgets/keyboard_layout_widget_definition.h"

namespace {

  settings::WidgetSettingVisibility glyphShown() { return {"show_glyph", {"true"}}; }

  settings::WidgetSettingVisibility labelShown() { return {"show_label", {"true"}}; }

} // namespace

const noctalia::bar::WidgetDefinition<KeyboardLayoutWidget::Options>& keyboardLayoutWidgetDefinition() {
  using noctalia::bar::field;
  using Options = KeyboardLayoutWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "keyboard_layout",
      .fields =
          {
              field<&Options::hideWhenSingleLayout>({
                  .key = "hide_when_single_layout",
              }),
              field<&Options::showGlyph>({
                  .key = "show_glyph",
              }),
              field<&Options::glyph>({
                  .key = "glyph",
                  .control = settings::WidgetControlKind::Glyph,
                  .presentation = settings::WidgetSettingPresentation{.visibleWhen = glyphShown()},
              }),
              field<&Options::customImage>({
                  .key = "custom_image",
                  .presentation = settings::WidgetSettingPresentation{.visibleWhen = glyphShown()},
              }),
              field<&Options::customImageColorize>({
                  .key = "custom_image_colorize",
                  .presentation = settings::WidgetSettingPresentation{.visibleWhen = glyphShown()},
              }),
              field<&Options::showLabel>({
                  .key = "show_label",
              }),
              field<&Options::display>({
                  .key = "display",
                  .choices =
                      {
                          {
                              .value = KeyboardLayoutDisplayMode::Short,
                              .configValue = "short",
                              .labelKey = "settings.widgets.options.short",
                          },
                          {
                              .value = KeyboardLayoutDisplayMode::Full,
                              .configValue = "full",
                              .labelKey = "settings.widgets.options.full",
                          },
                      },
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .segmented = true,
                          .visibleWhen = labelShown(),
                      },
              }),
          },
      .glyph = [](const Options& options) { return options.glyph; },
      .validateOptions = [](const Options& options) -> std::optional<std::string> {
        if (!options.showGlyph && !options.showLabel) {
          return "show_glyph and show_label cannot both be false";
        }
        return std::nullopt;
      },
  };
  return definition;
}
