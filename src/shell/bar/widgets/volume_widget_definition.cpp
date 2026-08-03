#include "shell/bar/widgets/volume_widget_definition.h"

const noctalia::bar::WidgetDefinition<VolumeWidget::Options>& volumeWidgetDefinition() {
  using noctalia::bar::field;
  using Options = VolumeWidget::Options;

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
              field<&Options::glyph>({
                  .key = "glyph",
                  .control = settings::WidgetControlKind::Glyph,
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .descriptionKey = "settings.widgets.settings.glyph.volume-description",
                      },
              }),
              field<&Options::muteGlyph>({
                  .key = "mute_glyph",
                  .control = settings::WidgetControlKind::Glyph,
              }),
              field<&Options::effectsProfileGlyphs>({
                  .key = "effects_profile_glyphs",
              }),
              field<&Options::customImage>({
                  .key = "custom_image",
              }),
              field<&Options::customImageColorize>({
                  .key = "custom_image_colorize",
              }),
              field<&Options::showLabel>({
                  .key = "show_label",
              }),
              field<&Options::muteColor>({
                  .key = "mute_color",
              }),
          },
      .glyph = [](const Options& options) -> std::string {
        if (!options.glyph.empty()) {
          return options.glyph;
        }
        return options.device == VolumeWidgetTarget::Input ? std::string("microphone") : std::string{};
      },
  };
  return definition;
}
