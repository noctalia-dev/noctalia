#include "shell/bar/widgets/media_widget_definition.h"

const noctalia::bar::WidgetDefinition<MediaWidget::Options>& mediaWidgetDefinition() {
  using noctalia::bar::field;
  using Options = MediaWidget::Options;

  static const settings::WidgetSettingVisibility notAlbumArtOnly{"album_art_only", {"false"}};
  static const settings::WidgetSettingVisibility notHideAlbumArt{"hide_album_art", {"false"}};

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "media",
      .fields = {
          field<&Options::albumArtOnly>({
              .key = "album_art_only",
              .presentation =
                  settings::WidgetSettingPresentation{
                      .visibleWhen = notHideAlbumArt,
                      .horizontalBarOnly = true,
                  },
          }),
          field<&Options::hideAlbumArt>({
              .key = "hide_album_art",
              .presentation =
                  settings::WidgetSettingPresentation{
                      .visibleWhen = notAlbumArtOnly,
                      .horizontalBarOnly = true,
                  },
          }),
          field<&Options::hideArtist>({
              .key = "hide_artist",
              .presentation =
                  settings::WidgetSettingPresentation{
                      .visibleWhen = notAlbumArtOnly,
                      .horizontalBarOnly = true,
                  },
          }),
          field<&Options::artistFirst>({
              .key = "artist_first",
              .presentation =
                  settings::WidgetSettingPresentation{
                      .visibleWhen = notAlbumArtOnly,
                      .horizontalBarOnly = true,
                  },
          }),
          field<&Options::minWidth>({
              .key = "min_length",
              .minValue = 0.0,
              .maxValue = 800.0,
              .step = 1.0,
              .presentation =
                  settings::WidgetSettingPresentation{
                      .visibleWhen = notAlbumArtOnly,
                  },
          }),
          field<&Options::maxWidth>({
              .key = "max_length",
              .minValue = 40.0,
              .maxValue = 800.0,
              .step = 1.0,
              .presentation =
                  settings::WidgetSettingPresentation{
                      .visibleWhen = notAlbumArtOnly,
                  },
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
              .presentation =
                  settings::WidgetSettingPresentation{
                      .visibleWhen = notAlbumArtOnly,
                  },
          }),
          field<&Options::hideWhenNoMedia>({
              .key = "hide_when_no_media",
          }),
      },
  };
  return definition;
}
