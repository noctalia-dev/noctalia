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
              field<&Options::matchAdjacentSpacing>({
                  .key = "match_adjacent_spacing",
              }),
              field<&Options::drawerMode>({
                  .key = "drawer",
              }),
              field<&Options::panelGridColumns>({
                  .key = "drawer_columns",
                  .minValue = 1.0,
                  .maxValue = 5.0,
                  .step = 1.0,
                  .presentation =
                      settings::WidgetSettingPresentation{
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
                          .visibleWhen = drawerOn,
                      },
              }),
              field<&Options::detachedPanel>({
                  .key = "detached_panel",
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .visibleWhen = drawerOn,
                      },
              }),
          },
      .finalize = [](Options& options, const TrayWidgetDefinitionContext& context) {
        options.barPosition = context.barPosition;
        options.inlineEntryGap = context.inlineEntryGap;
      },
  };
  return definition;
}
