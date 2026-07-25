#pragma once

#include "shell/settings/settings_registry.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Flex;
class InputArea;
class Label;
class Node;
class ConfigService;

namespace settings {

  // Pango line budget for setting descriptions: wrap up to this many lines, then ellipsize.
  inline constexpr int kSettingDescriptionMaxLines = 5;

  [[nodiscard]] std::unique_ptr<Label> makeSettingSubtitleLabel(std::string_view text, float scale);

  struct SearchPickerOpenRequest {
    std::string title;
    std::vector<SelectOption> options;
    std::string selectedValue;
    std::string placeholder;
    std::string emptyText;
    std::vector<std::string> settingPath;
  };

  struct SettingsContentContext {
    const Config& config;
    ConfigService* configService = nullptr;
    float scale = 1.0f;
    std::string_view searchQuery;
    std::string_view selectedSection;
    const BarConfig* selectedBar = nullptr;
    const BarMonitorOverride* selectedMonitorOverride = nullptr;
    bool showAdvanced = false;
    bool showOverriddenOnly = false;
    std::vector<SelectOption> batteryDeviceOptions;
    std::vector<std::string> keyboardLayoutNames;

    std::string& editingWidgetName;
    std::string& editingCapsuleGroupId;
    std::vector<std::string>& selectedLaneWidgets;
    std::string& pendingDeleteWidgetName;
    std::string& pendingDeleteWidgetSettingPath;
    std::string& renamingWidgetName;

    std::function<void()> requestRebuild;
    std::function<void()> requestContentRebuild;
    std::function<void()> resetContentScroll;
    std::function<void(Node*)> setScrollTarget;
    std::function<void(InputArea*)> focusArea;
    std::function<void(const std::vector<std::string>&)> openBarWidgetAddPopup;
    std::function<void(SearchPickerOpenRequest request)> openSearchPickerPopup;
    std::function<void(std::vector<std::string>, ConfigOverrideValue)> setOverride;
    std::function<void(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>)> setOverrides;
    std::function<void(std::vector<std::string>)> clearOverride;
    std::function<void(std::vector<std::vector<std::string>>)> clearOverrides;
    std::function<bool(const std::vector<std::vector<std::string>>&)> isResetConfirmationPending;
    std::function<void(std::vector<std::vector<std::string>>)> requestResetConfirmation;
    std::function<void(std::string, std::string, std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>)>
        renameWidgetInstance;

    std::function<void(std::size_t)> openSessionActionEntryEditor;
    std::function<void(std::size_t)> openIdleBehaviorEntryEditor;
    std::function<void()> openIdleBehaviorCreateEditor;
    std::function<void(std::size_t)> openNotificationFilterEntryEditor;
    std::function<void()> openNotificationFilterCreateEditor;
    std::function<void(std::vector<std::string> laneListPath, std::string widgetName)> openWidgetInspectorEditor;
    std::function<void(std::vector<std::string> laneListPath, std::string groupId)> openCapsuleGroupEditor;

    std::function<void(Label*)> registerIdleLiveStatusLabel;
    std::function<void(std::size_t, Label*)> registerSessionActionSummaryLabel;
    std::function<void(std::shared_ptr<std::vector<SessionPanelActionConfig>>)> bindSessionActionsEditState;

    // When set (session action entry popup), called after commits instead of requestRebuild.
    std::function<void()> afterSessionActionsCommit;
    std::function<void()> afterIdleBehaviorApply;
    std::function<void()> afterNotificationFilterApply;
    std::function<void()> closeHostedEditor;
    bool supportsTaskbarWorkspaceGrouping = true;
  };

  std::size_t
  addSettingsContentSections(Flex& content, const std::vector<SettingEntry>& registry, SettingsContentContext ctx);

  void buildSessionActionEntryDetailContent(
      Flex& parent, SettingsContentContext& ctx, SessionPanelActionConfig& row, const std::function<void()>& persist
  );
  void buildIdleBehaviorEntryDetailContent(
      Flex& parent, SettingsContentContext& ctx, IdleBehaviorConfig& row, const std::function<void()>& persist
  );
  void buildNotificationFilterEntryDetailContent(
      Flex& parent, SettingsContentContext& ctx, NotificationFilterConfig& row, const std::function<void()>& persist
  );

} // namespace settings
