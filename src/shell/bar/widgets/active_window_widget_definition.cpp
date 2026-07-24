#include "shell/bar/widgets/active_window_widget_definition.h"

const noctalia::bar::WidgetDefinition<ActiveWindowWidget::Options>& activeWindowWidgetDefinition() {
  using noctalia::bar::field;
  using Options = ActiveWindowWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "active_window",
      .fields = {
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
          field<&Options::iconSize>({
              .key = "icon_size",
              .minValue = 8.0,
              .maxValue = 64.0,
              .step = 1.0,
          }),
          field<&Options::titleScrollMode>({
              .key = "title_scroll",
              .choices =
                  {
                      {
                          .value = ActiveWindowTitleScrollMode::None,
                          .configValue = "none",
                          .labelKey = "settings.widgets.options.none",
                      },
                      {
                          .value = ActiveWindowTitleScrollMode::Always,
                          .configValue = "always",
                          .labelKey = "settings.widgets.options.always",
                      },
                      {
                          .value = ActiveWindowTitleScrollMode::OnHover,
                          .configValue = "on_hover",
                          .labelKey = "settings.widgets.options.on-hover",
                      },
                  },
          }),
          field<&Options::displayMode>({
              .key = "display",
              .choices =
                  {
                      {
                          .value = ActiveWindowDisplayMode::IconAndText,
                          .configValue = "icon_and_text",
                          .labelKey = "settings.widgets.options.icon-and-text",
                      },
                      {
                          .value = ActiveWindowDisplayMode::IconOnly,
                          .configValue = "icon_only",
                          .labelKey = "settings.widgets.options.icon-only",
                      },
                      {
                          .value = ActiveWindowDisplayMode::TextOnly,
                          .configValue = "text_only",
                          .labelKey = "settings.widgets.options.text-only",
                      },
                  },
              .presentation =
                  settings::WidgetSettingPresentation{
                      .descriptionKey = "settings.widgets.settings.display.active-window-description",
                  },
          }),
          field<&Options::showEmptyLabel>({
              .key = "show_empty_label",
          }),
      },
  };
  return definition;
}
