#include "shell/bar/widgets/taskbar_widget_definition.h"

namespace {

  settings::WidgetSettingVisibility matches(std::string key, std::vector<std::string> values) {
    return settings::WidgetSettingVisibility{
        settings::WidgetSettingVisibilityCondition{std::move(key), std::move(values)}
    };
  }

  settings::WidgetSettingVisibility all(std::initializer_list<settings::WidgetSettingVisibilityCondition> conditions) {
    settings::WidgetSettingVisibility visibility;
    visibility.all = conditions;
    return visibility;
  }

  settings::WidgetSettingPresentation presentation(
      std::string group, std::optional<settings::WidgetSettingVisibility> visibleWhen = std::nullopt,
      bool requiresWorkspaceGrouping = false, std::string descriptionKey = {}, std::string labelKey = {}
  ) {
    settings::WidgetSettingPresentation result;
    result.group = std::move(group);
    result.visibleWhen = std::move(visibleWhen);
    result.descriptionKey = std::move(descriptionKey);
    result.labelKey = std::move(labelKey);
    if (requiresWorkspaceGrouping) {
      result.requiresCapability = settings::WidgetSettingCapability::TaskbarWorkspaceGrouping;
    }
    return result;
  }

  settings::WidgetSettingVisibility groupedOnly() { return matches("group_by_workspace", {"true"}); }

} // namespace

const noctalia::bar::WidgetDefinition<TaskbarWidgetOptions>& taskbarWidgetDefinition() {
  using noctalia::bar::field;
  using Options = TaskbarWidgetOptions;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "taskbar",
      .fields =
          {
              field<&Options::showAllOutputs>({
                  .key = "show_all_outputs",
                  .presentation = presentation("taskbar.windows"),
              }),
              field<&Options::showActiveIndicator>({
                  .key = "show_active_indicator",
                  .presentation = presentation("taskbar.windows"),
              }),
              field<&Options::activeIndicatorColor>({
                  .key = "active_indicator_color",
                  .presentation = presentation("taskbar.windows", matches("show_active_indicator", {"true"})),
              }),
              field<&Options::activeOpacity>({
                  .key = "active_opacity",
                  .minValue = 0.1,
                  .maxValue = 1.0,
                  .step = 0.01,
                  .presentation = presentation("taskbar.windows"),
              }),
              field<&Options::inactiveOpacity>({
                  .key = "inactive_opacity",
                  .minValue = 0.1,
                  .maxValue = 1.0,
                  .step = 0.01,
                  .presentation = presentation("taskbar.windows"),
              }),
              field<&Options::pinned>({
                  .key = "pinned",
                  .presentation = presentation(
                      "taskbar.windows", matches("group_by_workspace", {"false"}), false,
                      "settings.widgets.settings.pinned.taskbar-description",
                      "settings.widgets.settings.pinned.taskbar-label"
                  ),
              }),
              field<&Options::pinnedOpacity>({
                  .key = "pinned_opacity",
                  .minValue = 0.0,
                  .maxValue = 1.0,
                  .step = 0.01,
                  .presentation = presentation(
                      "taskbar.windows",
                      all({
                          {"pinned", {}, true},
                          {"group_by_workspace", {"false"}},
                      })
                  ),
              }),
              field<&Options::showWindowTitle>({
                  .key = "show_window_title",
                  .presentation = presentation("taskbar.windows", matches("group_by_workspace", {"false"})),
              }),
              field<&Options::windowTitleMaxWidth>({
                  .key = "window_title_max_width",
                  .minValue = 10.0,
                  .maxValue = 200.0,
                  .step = 1.0,
                  .presentation = presentation(
                      "taskbar.windows",
                      all({
                          {"show_window_title", {"true"}},
                          {"group_by_workspace", {"false"}},
                      })
                  ),
              }),
              field<&Options::taskbarMaxWidth>({
                  .key = "taskbar_max_width",
                  .minValue = 10.0,
                  .maxValue = 8192.0,
                  .step = 1.0,
                  .presentation = presentation(
                      "taskbar.windows",
                      all({
                          {"show_window_title", {"true"}},
                          {"group_by_workspace", {"false"}},
                      })
                  ),
              }),
              field<&Options::onlyActiveWorkspace>({
                  .key = "only_active_workspace",
                  .presentation = presentation("taskbar.grouping", std::nullopt, true),
              }),
              field<&Options::groupByWorkspace>({
                  .key = "group_by_workspace",
                  .presentation = presentation("taskbar.grouping", std::nullopt, true),
              }),
              field<&Options::hideEmptyWorkspaces>({
                  .key = "hide_empty_workspaces",
                  .presentation = presentation("taskbar.grouping", groupedOnly(), true),
              }),
              field<&Options::workspaceGroupContent>(
                  {
                      .key = "workspace_group_content",
                      .choices =
                          {
                              {
                                  .value = WorkspaceGroupContent::Icons,
                                  .configValue = "icons",
                                  .labelKey = "settings.widgets.options.icons",
                              },
                              {
                                  .value = WorkspaceGroupContent::Count,
                                  .configValue = "count",
                                  .labelKey = "settings.widgets.options.count",
                              },
                              {
                                  .value = WorkspaceGroupContent::Dots,
                                  .configValue = "dots",
                                  .labelKey = "settings.widgets.options.dots",
                              },
                          },
                      .presentation = presentation("taskbar.grouping", groupedOnly(), true),
                  }
              ),
              field<&Options::groupSingleIconPerApp>({
                  .key = "group_single_icon_per_app",
                  .presentation = presentation(
                      "taskbar.grouping",
                      all({
                          {"group_by_workspace", {"true"}},
                          {"workspace_group_content", {"icons"}},
                      }),
                      true
                  ),
              }),
              field<&Options::workspaceGroupCapsule>({
                  .key = "workspace_group_capsule",
                  .presentation = presentation(
                      "taskbar.grouping", groupedOnly(), true,
                      "settings.widgets.settings.workspace-group-capsule.description"
                  ),
              }),
              field<&Options::showWorkspaceLabel>({
                  .key = "show_workspace_label",
                  .presentation = presentation("taskbar.workspace-labels", groupedOnly(), true),
              }),
              field<&Options::workspaceLabelPlacement>(
                  {
                      .key = "workspace_label_placement",
                      .choices =
                          {
                              {
                                  .value = WorkspaceLabelPlacement::Corner,
                                  .configValue = "corner",
                                  .labelKey = "settings.widgets.options.workspace-label-corner",
                              },
                              {
                                  .value = WorkspaceLabelPlacement::Centered,
                                  .configValue = "centered",
                                  .labelKey = "settings.widgets.options.workspace-label-centered",
                              },
                              {
                                  .value = WorkspaceLabelPlacement::Inside,
                                  .configValue = "inside",
                                  .labelKey = "settings.widgets.options.workspace-label-inside",
                              },
                          },
                      .presentation = presentation("taskbar.workspace-labels", groupedOnly(), true),
                  }
              ),
              field<&Options::minimal>({
                  .key = "minimal",
                  .presentation = presentation(
                      "taskbar.workspace-labels",
                      all({
                          {"group_by_workspace", {"true"}},
                          {"show_workspace_label", {"true"}},
                      }),
                      true, "settings.widgets.settings.minimal.taskbar-description"
                  ),
              }),
              field<&Options::focusedOutputOnly>({
                  .key = "focused_output_only",
                  .presentation = presentation(
                      "taskbar.workspace-labels",
                      all({
                          {"group_by_workspace", {"true"}},
                          {"show_workspace_label", {"true"}},
                      }),
                      true, "settings.widgets.settings.focused-output-only.taskbar-description"
                  ),
              }),
              field<&Options::focusedColor>({
                  .key = "focused_color",
                  .presentation = presentation("taskbar.workspace-labels", groupedOnly(), true),
              }),
              field<&Options::occupiedColor>({
                  .key = "occupied_color",
                  .presentation = presentation("taskbar.workspace-labels", groupedOnly(), true),
              }),
              field<&Options::emptyColor>({
                  .key = "empty_color",
                  .presentation = presentation("taskbar.workspace-labels", groupedOnly(), true),
              }),
              field<&Options::urgentColor>({
                  .key = "urgent_color",
                  .presentation = presentation("taskbar.workspace-labels", groupedOnly(), true),
              }),
          },
      .commonOverrides = {
          {
              .key = "capsule_radius",
              .descriptionKey = "settings.widgets.settings.capsule-radius.taskbar-description",
              .replaceVisibleWhen = settings::WidgetSettingVisibility{
                  {"capsule", {"true"}},
                  {"group_by_workspace", {"true"}},
              },
          },
      },
  };
  return definition;
}
