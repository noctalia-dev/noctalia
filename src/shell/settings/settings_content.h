#pragma once

#include "config/config_service.h"
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
class Button;
class Label;
class Node;

namespace settings {

  // Pango line budget for setting descriptions: wrap up to this many lines, then ellipsize.
  inline constexpr int kSettingDescriptionMaxLines = 5;

  [[nodiscard]] std::unique_ptr<Label> makeSettingSubtitleLabel(std::string_view text, float scale);

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

    std::string& editingWidgetName;
    std::string& pendingDeleteWidgetName;
    std::string& pendingDeleteWidgetSettingPath;
    std::string& renamingWidgetName;

    std::function<void()> requestRebuild;
    std::function<void()> requestContentRebuild;
    std::function<void()> resetContentScroll;
    std::function<void(Node*)> setScrollTarget;
    std::function<void(InputArea*)> focusArea;
    std::function<void(const std::vector<std::string>&)> openBarWidgetAddPopup;
    std::function<void(
        const std::string& title, const std::vector<SelectOption>& options, const std::string& selectedValue,
        const std::string& placeholder, const std::string& emptyText, const std::vector<std::string>& settingPath
    )>
        openSearchPickerPopup;
    std::function<void(std::vector<std::string>, ConfigOverrideValue)> setOverride;
    std::function<void(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>)> setOverrides;
    std::function<void(std::vector<std::string>)> clearOverride;
    std::function<void(std::string, std::string, std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>)>
        renameWidgetInstance;

    std::function<void(std::size_t)> openSessionActionEntryEditor;
    std::function<void(std::size_t)> openIdleBehaviorEntryEditor;
    std::function<void()> openIdleBehaviorCreateEditor;

    std::function<void(Label*)> registerIdleLiveStatusLabel;

    // When set (session action entry popup), called after commits instead of requestRebuild.
    std::function<void()> afterSessionActionsCommit;
    std::function<void()> afterIdleBehaviorApply;
    std::function<void()> closeHostedEditor;
  };

  std::size_t
  addSettingsContentSections(Flex& content, const std::vector<SettingEntry>& registry, SettingsContentContext ctx);

  void buildSessionActionEntryDetailContent(
      Flex& parent, SettingsContentContext& ctx, SessionPanelActionConfig& row, const std::function<void()>& persist
  );
  void buildIdleBehaviorEntryDetailContent(
      Flex& parent, SettingsContentContext& ctx, IdleBehaviorConfig& row, const std::function<void()>& persist
  );

} // namespace settings
