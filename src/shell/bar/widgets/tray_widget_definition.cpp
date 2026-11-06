#include "shell/bar/widgets/tray_widget_definition.h"

#include "shell/bar/glyph_value_definition.h"

const noctalia::bar::WidgetDefinition<TrayWidget::Options, TrayWidgetDefinitionContext>& trayWidgetDefinition() {
  using noctalia::bar::field;
  using Options = TrayWidget::Options;
  using Definition = noctalia::bar::WidgetDefinition<Options, TrayWidgetDefinitionContext>;

  static const settings::WidgetSettingVisibility drawerOn{"drawer", {"true"}};
  static const settings::WidgetSettingVisibility drawerOff{"drawer", {"false"}};
  static const settings::WidgetSettingVisibility glyphVisible = [] {
    settings::WidgetSettingVisibility visibility;
    visibility.any = {
        {.key = "drawer", .values = {"true"}},
        {.key = "peek", .values = {"true"}},
    };
    return visibility;
  }();
  static const settings::WidgetSettingVisibility detachedDrawerOn = [] {
    settings::WidgetSettingVisibility visibility;
    visibility.all = {
        settings::WidgetSettingVisibilityCondition{"drawer", {"true"}},
        settings::WidgetSettingVisibilityCondition{"detached_panel", {"true"}},
    };
    return visibility;
  }();

  static const Definition definition{
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
              noctalia::bar::peekField<Options>(drawerOff),
              field<&Options::glyph>({
                  .key = "glyph",
                  .control = settings::WidgetControlKind::Glyph,
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .descriptionKey = "settings.widgets.settings.glyph.tray-description",
                          .visibleWhen = glyphVisible,
                      },
              }),
              noctalia::bar::glyphPositionField<Options>(glyphVisible),
              noctalia::bar::peekDelayField<Options>(drawerOff),
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
              field<&Options::drawerBackgroundOpacity>({
                  .key = "drawer_background_opacity",
                  .minValue = 0.0,
                  .maxValue = 1.0,
                  .step = 0.01,
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .group = "tray.drawer",
                          .visibleWhen = detachedDrawerOn,
                      },
                  .defaultFromConfig = [](const Config& cfg) -> WidgetSettingValue {
                    return static_cast<double>(
                        detachedPanelBackgroundOpacityForTransparencyMode(cfg.shell.panel.transparencyMode)
                    );
                  },
              }),
              field<&Options::iconOverrideBlacklist>({
                  .key = "icon_override_blacklist",
                  .presentation = settings::WidgetSettingPresentation{.group = "tray.icon-overrides"},
              }),
              field<&Options::colorizeIcons>({
                  .key = "colorize_icons",
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
