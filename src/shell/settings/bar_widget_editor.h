#pragma once

#include "shell/settings/settings_registry.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Button;
class ConfigService;
class Flex;
class InputArea;
class Node;

namespace settings {

  class SettingsControlFactory;

  struct BarWidgetEditorContext {
    const Config& config;
    ConfigService* configService = nullptr;
    float scale = 1.0f;
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
    std::function<void()> resetContentScroll;
    std::function<void(Node*)> setScrollTarget;
    std::function<void(InputArea*)> focusArea;
    std::function<void(const std::vector<std::string>&)> openWidgetAddPopup;
    std::function<void(std::vector<std::string>, ConfigOverrideValue)> setOverride;
    std::function<void(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>)> setOverrides;
    std::function<void(std::vector<std::string>)> clearOverride;
    std::function<void(std::string, std::string, std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>)>
        renameWidgetInstance;
    std::function<void()> closeHostedEditor;
    std::function<void(std::vector<std::string> laneListPath, std::string widgetName)> openWidgetInspector;
    std::function<void(std::vector<std::string> laneListPath, std::string groupId)> openCapsuleGroupInspector;
    std::function<std::unique_ptr<Button>(const std::vector<std::string>&)> makeResetButton;
    std::function<void(Flex&, const SettingEntry&, std::unique_ptr<Node>)> makeRow;
    std::function<std::unique_ptr<Node>(bool, std::vector<std::string>, std::optional<bool> clearWhenValue)> makeToggle;
    std::function<std::unique_ptr<Node>(const SelectSetting&, std::vector<std::string>)> makeSelect;
    std::function<std::unique_ptr<Node>(const SearchPickerSetting&, std::string, std::vector<std::string>)>
        makeSearchPicker;
    std::function<std::unique_ptr<Node>(double, double, double, double, std::vector<std::string>, bool)> makeSlider;
    std::function<std::unique_ptr<Node>(const OptionalNumberSetting&, std::vector<std::string>)> makeOptionalNumber;
    std::function<std::unique_ptr<Node>(const OptionalStepperSetting&, std::vector<std::string>)> makeOptionalStepper;
    std::function<std::unique_ptr<Node>(const StepperSetting&, std::vector<std::string>)> makeStepper;
    std::function<std::unique_ptr<Node>(const std::string&, const std::string&, std::vector<std::string>)> makeText;
    std::function<std::unique_ptr<Node>(const ColorSpecPickerSetting&, std::vector<std::string>)> makeColorSpecPicker;
    std::function<void(Flex&, const SettingEntry&, const ListSetting&)> makeListBlock;
    std::function<void(Flex&, const SettingEntry&, const StringMapSetting&)> makeStringMapBlock;
    bool supportsTaskbarWorkspaceGrouping = true;
  };

  [[nodiscard]] bool isBarWidgetListPath(const std::vector<std::string>& path);
  [[nodiscard]] bool isFirstBarWidgetListPath(const std::vector<std::string>& path);

  void addBarWidgetLaneEditor(Flex& section, const SettingEntry& entry, const BarWidgetEditorContext& ctx);

  // Builds a BarWidgetEditorContext whose control factories delegate to `factory`. The returned
  // context references `factory` and `factory.context()`, so `factory` must outlive it.
  [[nodiscard]] BarWidgetEditorContext makeBarWidgetEditorContext(SettingsControlFactory& factory);

  // Builds the bar-widget settings inspector for `ctx.editingWidgetName` into `body`. `laneListPath`
  // is the first-lane bar-widget list path (e.g. {"bar", name, "start"}) used to resolve lane moves.
  void
  buildWidgetInspectorBody(Flex& body, const std::vector<std::string>& laneListPath, const BarWidgetEditorContext& ctx);
  // Builds the capsule-group style editor for `ctx.editingCapsuleGroupId` into `body`. `laneListPath`
  // identifies the owning bar (path[1]).
  void
  buildCapsuleGroupBody(Flex& body, const std::vector<std::string>& laneListPath, const BarWidgetEditorContext& ctx);

} // namespace settings
