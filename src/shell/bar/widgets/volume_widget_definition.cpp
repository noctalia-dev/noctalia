#include "shell/bar/widgets/volume_widget_definition.h"

#include "shell/bar/glyph_value_definition.h"

const noctalia::bar::WidgetDefinition<VolumeWidget::Options>& volumeWidgetDefinition() {
  using noctalia::bar::field;
  using noctalia::bar::glyphPositionField;
  using noctalia::bar::peekDelayField;
  using noctalia::bar::peekField;
  using noctalia::bar::whenAllTrue;
  using Options = VolumeWidget::Options;

  static const settings::WidgetSettingVisibility showGlyph{"show_glyph", {"true"}};

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "volume",
      .fields =
          {
              field<&Options::device>({
                  .key = "device",
                  .choices =
                      {
                          {
                              .value = VolumeWidgetTarget::Output,
                              .configValue = "output",
                              .labelKey = "settings.widgets.options.output",
                          },
                          {
                              .value = VolumeWidgetTarget::Input,
                              .configValue = "input",
                              .labelKey = "settings.widgets.options.input",
                          },
                      },
                  .presentation = settings::WidgetSettingPresentation{.segmented = true},
              }),
              field<&Options::showGlyph>({
                  .key = "show_glyph",
              }),
              field<&Options::glyph>({
                  .key = "glyph",
                  .control = settings::WidgetControlKind::Glyph,
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .descriptionKey = "settings.widgets.settings.glyph.volume-description",
                          .visibleWhen = showGlyph,
                      },
              }),
              field<&Options::muteGlyph>({
                  .key = "mute_glyph",
                  .control = settings::WidgetControlKind::Glyph,
                  .presentation = settings::WidgetSettingPresentation{.visibleWhen = showGlyph},
              }),
              field<&Options::effectsProfileGlyphs>({
                  .key = "effects_profile_glyphs",
                  .presentation = settings::WidgetSettingPresentation{.visibleWhen = showGlyph},
              }),
              field<&Options::customImage>({
                  .key = "custom_image",
                  .presentation = settings::WidgetSettingPresentation{.visibleWhen = showGlyph},
              }),
              field<&Options::customImageColorize>({
                  .key = "custom_image_colorize",
                  .presentation = settings::WidgetSettingPresentation{.visibleWhen = showGlyph},
              }),
              field<&Options::showLabel>({
                  .key = "show_label",
              }),
              glyphPositionField<Options>(whenAllTrue({"show_glyph", "show_label"})),
              peekField<Options>(whenAllTrue({"show_glyph", "show_label"})),
              peekDelayField<Options>(whenAllTrue({"show_glyph", "show_label"})),
              field<&Options::muteColor>({
                  .key = "mute_color",
              }),
              field<&Options::hideWhenInactive>({
                  .key = "hide_when_inactive",
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .visibleWhen = settings::WidgetSettingVisibility{"device", {"input"}},
                      },
              }),
          },
      .glyph = [](const Options& options) -> std::string {
        if (!options.glyph.empty()) {
          return options.glyph;
        }
        return options.device == VolumeWidgetTarget::Input ? std::string("microphone") : std::string{};
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
