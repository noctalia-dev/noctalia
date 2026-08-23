#include "shell/bar/widgets/workspaces_widget_definition.h"

namespace {

  settings::WidgetSettingVisibility labelsShown() {
    settings::WidgetSettingVisibility visibility;
    visibility.all = {{"show_labels", {"true"}}};
    return visibility;
  }

  // FocusHint labels the focused workspace only, so the occupied filter never bites there.
  settings::WidgetSettingVisibility occupiedFilterApplies() {
    settings::WidgetSettingVisibility visibility;
    visibility.all = {
        {"show_labels", {"true"}},
        {"style", {"regular", "minimal"}},
    };
    return visibility;
  }

  settings::WidgetSettingVisibility regularStyleOnly() {
    settings::WidgetSettingVisibility visibility;
    visibility.all = {{"style", {"regular"}}};
    return visibility;
  }

} // namespace

const noctalia::bar::WidgetDefinition<WorkspacesWidget::Options>& workspacesWidgetDefinition() {
  using noctalia::bar::field;
  using Options = WorkspacesWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "workspaces",
      .fields =
          {
              field<&Options::hideWhenEmpty>({
                  .key = "hide_when_empty",
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .descriptionKey = "settings.widgets.settings.hide-when-empty.workspaces-description",
                          .group = "workspaces.list",
                      },
              }),
              field<&Options::showAllOutputs>({
                  .key = "show_all_outputs",
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .labelKey = "settings.widgets.settings.show-all-workspaces.label",
                          .descriptionKey = "settings.widgets.settings.show-all-workspaces.description",
                          .group = "workspaces.list",
                      },
              }),
              field<&Options::showLabels>({
                  .key = "show_labels",
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .group = "workspaces.list",
                      },
              }),
              field<&Options::labelSource>({
                  .key = "label_source",
                  .choices =
                      {
                          {
                              .value = WorkspacesLabelSource::Id,
                              .configValue = "id",
                              .labelKey = "settings.widgets.options.id",
                          },
                          {
                              .value = WorkspacesLabelSource::Name,
                              .configValue = "name",
                              .labelKey = "settings.widgets.options.name",
                          },
                      },
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .group = "workspaces.list",
                          .segmented = true,
                          .visibleWhen = labelsShown(),
                      },
              }),
              field<&Options::labelsOnlyWhenOccupied>({
                  .key = "labels_only_when_occupied",
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .descriptionKey =
                              "settings.widgets.settings.labels-only-when-occupied.workspaces-description",
                          .group = "workspaces.list",
                          .visibleWhen = occupiedFilterApplies(),
                      },
              }),
              field<&Options::maxLabelChars>({
                  .key = "max_label_chars",
                  .minValue = 1.0,
                  .maxValue = 20.0,
                  .step = 1.0,
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .descriptionKey = "settings.widgets.settings.max-label-chars.workspaces-description",
                          .group = "workspaces.list",
                          .visibleWhen = labelsShown(),
                      },
              }),
              field<&Options::style>({
                  .key = "style",
                  .choices =
                      {
                          {
                              .value = WorkspacesStyle::Regular,
                              .configValue = "regular",
                              .labelKey = "settings.widgets.options.regular",
                          },
                          {
                              .value = WorkspacesStyle::Minimal,
                              .configValue = "minimal",
                              .labelKey = "settings.widgets.options.minimal",
                          },
                          {
                              .value = WorkspacesStyle::FocusHint,
                              .configValue = "focus_hint",
                              .labelKey = "settings.widgets.options.focus-hint",
                          },
                      },
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .descriptionKey = "settings.widgets.settings.style.workspaces-description",
                          .group = "workspaces.pills",
                          .segmented = true,
                      },
              }),
              field<&Options::pillScale>({
                  .key = "pill_scale",
                  .minValue = 0.1,
                  .maxValue = 1.0,
                  .step = 0.05,
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .descriptionKey = "settings.widgets.settings.pill-scale.workspaces-description",
                          .group = "workspaces.pills",
                          .visibleWhen = regularStyleOnly(),
                      },
              }),
              field<&Options::activePillSize>({
                  .key = "active_pill_size",
                  .minValue = 0.25,
                  .maxValue = 8.0,
                  .step = 0.05,
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .descriptionKey = "settings.widgets.settings.active-pill-size.workspaces-description",
                          .group = "workspaces.pills",
                          .visibleWhen = regularStyleOnly(),
                      },
              }),
              field<&Options::inactivePillSize>({
                  .key = "inactive_pill_size",
                  .minValue = 0.25,
                  .maxValue = 8.0,
                  .step = 0.05,
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .descriptionKey = "settings.widgets.settings.inactive-pill-size.workspaces-description",
                          .group = "workspaces.pills",
                          .visibleWhen = regularStyleOnly(),
                      },
              }),
              field<&Options::focusedOutputOnly>({
                  .key = "focused_output_only",
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .descriptionKey = "settings.widgets.settings.focused-output-only.workspaces-description",
                          .group = "workspaces.colors",
                      },
              }),
              field<&Options::changeColorOnHover>({
                  .key = "change_color_on_hover",
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .group = "workspaces.colors",
                      },
              }),
              field<&Options::focusedColor>({
                  .key = "focused_color",
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .group = "workspaces.colors",
                      },
              }),
              field<&Options::occupiedColor>({
                  .key = "occupied_color",
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .group = "workspaces.colors",
                      },
              }),
              field<&Options::emptyColor>({
                  .key = "empty_color",
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .group = "workspaces.colors",
                      },
              }),
              field<&Options::urgentColor>({
                  .key = "urgent_color",
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .group = "workspaces.colors",
                      },
              }),
          },
      .commonOverrides =
          {
              {
                  .key = "capsule_radius",
                  .descriptionKey = "settings.widgets.settings.capsule-radius.workspaces-description",
                  .visibleWhen = regularStyleOnly(),
              },
          },
  };
  return definition;
}
