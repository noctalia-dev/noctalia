#include "shell/bar/widgets/tray_widget_definition.h"

const noctalia::bar::WidgetDefinition<TrayWidget::Options, TrayWidgetDefinitionContext>& trayWidgetDefinition() {
  using noctalia::bar::field;
  using Options = TrayWidget::Options;

  static const settings::WidgetSettingVisibility drawerOn{"drawer", {"true"}};

  static const noctalia::bar::WidgetDefinition<Options, TrayWidgetDefinitionContext> definition{
      .type = "tray",
      .fields =
          {
              field<&Options::hiddenItems>({
                  .key = "hidden",
              }),
              field<&Options::pinnedItems>({
                  .key = "pinned",
              }),
              field<&Options::hidePassive>({
                  .key = "hide_passive",
              }),
              field<&Options::matchAdjacentSpacing>({
                  .key = "match_adjacent_spacing",
              }),
              field<&Options::drawerMode>({
                  .key = "drawer",
                  .presentation = settings::WidgetSettingPresentation{.group = "tray.drawer"},
              }),
              field<&Options::panelGridColumns>({
                  .key = "drawer_columns",
                  .minValue = 1.0,
                  .maxValue = 5.0,
                  .step = 1.0,
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .group = "tray.drawer",
                          .visibleWhen = drawerOn,
                      },
              }),
              field<&Options::drawerItemSize>({
                  .key = "drawer_item_size",
                  .minValue = 8.0,
                  .maxValue = 64.0,
                  .step = 1.0,
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .group = "tray.drawer",
                          .visibleWhen = drawerOn,
                      },
              }),
              field<&Options::detachedPanel>({
                  .key = "detached_panel",
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .group = "tray.drawer",
                          .visibleWhen = drawerOn,
                      },
              }),
              field<&Options::iconOverrideBlacklist>({
                  .key = "icon_override_blacklist",
                  .presentation = settings::WidgetSettingPresentation{.group = "tray.icon-overrides"},
              }),
              field<&Options::iconType>({
                  .key = "icon_type",
                  .choices =
                      {
                          {
                              .value = TrayIconType::Default,
                              .configValue = "default",
                              .labelKey = "settings.widgets.options.default",
                          },
                          {
                              .value = TrayIconType::Desktop,
                              .configValue = "desktop",
                              .labelKey = "settings.widgets.options.desktop",
                          },
                          {
                              .value = TrayIconType::Symbolic,
                              .configValue = "symbolic",
                              .labelKey = "settings.widgets.options.symbolic",
                          },
                      },
                  .presentation =
                      settings::WidgetSettingPresentation{.group = "tray.icon-overrides", .segmented = true},
              }),
          },
      .finalize = [](Options& options, const TrayWidgetDefinitionContext& context) {
        options.barPosition = context.barPosition;
        options.inlineEntryGap = context.inlineEntryGap;
      },
  };
  return definition;
}
