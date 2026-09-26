#include "shell/bar/widgets/media_widget_definition.h"
const noctalia::bar::WidgetDefinition<MediaWidget::Options>& mediaWidgetDefinition() {
  using noctalia::bar::field;
  using Options = MediaWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "media",
      .fields = {
          field<&Options::hideArtist>({
              .key = "hide_artist",
              .presentation =
                  settings::WidgetSettingPresentation{
                      .labelKey = "settings.widgets.settings.media-hide-artist.label",
                      .descriptionKey = "settings.widgets.settings.media-hide-artist.description",
                      .horizontalBarOnly = true,
                  },
          }),
          field<&Options::hideTitle>({
              .key = "hide_title",
              .presentation =
                  settings::WidgetSettingPresentation{
                      .labelKey = "settings.widgets.settings.media-hide-title.label",
                      .descriptionKey = "settings.widgets.settings.media-hide-title.description",
                      .horizontalBarOnly = true,
                  },
          }),
          field<&Options::hideControls>({
              .key = "hide_controls",
              .presentation =
                  settings::WidgetSettingPresentation{
                      .labelKey = "settings.widgets.settings.media-hide-controls.label",
                      .descriptionKey = "settings.widgets.settings.media-hide-controls.description",
                      .horizontalBarOnly = true,
                  },
          }),
          field<&Options::hideAlbumArt>({
              .key = "hide_album_art",
              .presentation =
                  settings::WidgetSettingPresentation{
                      .labelKey = "settings.widgets.settings.media-hide-album-art.label",
                      .descriptionKey = "settings.widgets.settings.media-hide-album-art.description",
                      .horizontalBarOnly = true,
                  },
          }),
          field<&Options::minWidth>({
              .key = "min_length",
              .minValue = 0.0,
              .maxValue = 800.0,
              .step = 1.0,
          }),
          field<&Options::maxWidth>({
              .key = "max_length",
              .minValue = 40.0,
              .maxValue = 800.0,
              .step = 1.0,
          }),
          field<&Options::artSize>({
              .key = "art_size",
              .minValue = 8.0,
              .maxValue = 96.0,
              .step = 1.0,
          }),
          field<&Options::titleScrollMode>({
    .key = "title_scroll",
    .choices =
        {
            {
                .value = MediaTitleScrollMode::None,
                .configValue = "none",
                .labelKey = "settings.widgets.options.none",
            },
            {
                .value = MediaTitleScrollMode::Always,
                .configValue = "always",
                .labelKey = "settings.widgets.options.always",
            },
            {
                .value = MediaTitleScrollMode::OnHover,
                .configValue = "on_hover",
                .labelKey = "settings.widgets.options.on-hover",
            },
        },
}),
field<&Options::scrollSpeed>({
    .key = "scroll_speed",
    .minValue = 1.0,
    .maxValue = 200.0,
    .step = 1.0,
}),
field<&Options::showProgress>({
    .key = "show_progress",
    .presentation =
        settings::WidgetSettingPresentation{
            .horizontalBarOnly = true,
        },
}),
field<&Options::hideWhenNoMedia>({
    .key = "hide_when_no_media",
}),
      },
  };

  return definition;
}
