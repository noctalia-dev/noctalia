#pragma once

#include "shell/settings/settings_content.h"
#include "shell/settings/settings_registry.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Button;
class Flex;
class Input;
class Node;
class Stepper;
class Toggle;

namespace settings {

  // Builds the override-backed settings controls (rows, toggles, sliders, selects, color
  // pickers, list editors, ...). Owns a copy of the SettingsContentContext so it can be
  // constructed once and reused across independent render passes — both the main settings
  // list and the bar-widget inspector popup build their controls through one of these.
  class SettingsControlFactory {
  public:
    explicit SettingsControlFactory(SettingsContentContext ctx);

    [[nodiscard]] const SettingsContentContext& context() const noexcept { return m_ctx; }
    [[nodiscard]] float scale() const noexcept { return m_scale; }

    [[nodiscard]] std::unique_ptr<Button> makeResetButton(const std::vector<std::string>& path);

    void makeRow(Flex& section, const SettingEntry& entry, std::unique_ptr<Node> control);

    [[nodiscard]] std::unique_ptr<Toggle> makeToggle(
        bool checked, bool enabled, std::vector<std::string> path, std::optional<bool> clearWhenValue = std::nullopt
    );

    [[nodiscard]] std::unique_ptr<Node> makeSelect(const SelectSetting& setting, std::vector<std::string> path);

    [[nodiscard]] std::unique_ptr<Flex> makeSlider(
        double value, double minValue, double maxValue, double step, std::vector<std::string> path,
        bool integerValue = false,
        std::function<std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>(double)> linkedCommit = {}
    );

    [[nodiscard]] std::unique_ptr<Input> makeText(
        const std::string& value, const std::string& placeholder, std::vector<std::string> path, float width = 0.0f
    );

    [[nodiscard]] std::unique_ptr<Input>
    makeOptionalNumber(const OptionalNumberSetting& setting, std::vector<std::string> path);

    [[nodiscard]] std::unique_ptr<Stepper> makeStepper(const StepperSetting& setting, std::vector<std::string> path);

    [[nodiscard]] std::unique_ptr<Flex>
    makeOptionalStepper(const OptionalStepperSetting& setting, std::vector<std::string> path);

    [[nodiscard]] std::unique_ptr<Node>
    makeColorSpecPicker(const ColorSpecPickerSetting& setting, std::vector<std::string> path);

    [[nodiscard]] std::unique_ptr<Flex> makeCollectionBlock(
        const SettingEntry& entry, bool overridden, bool reserveTitleHeight = false, bool titleMaxTwoLines = false,
        bool fillWidth = false, bool flexGrow = false, bool compactTitleDescription = false
    );

    void makeListBlock(Flex& section, const SettingEntry& entry, const ListSetting& list);
    void makeKeyValueMapBlock(Flex& section, const SettingEntry& entry, const KeyValueMapSetting& map);

  private:
    [[nodiscard]] std::unique_ptr<Flex>
    makeStatusBadge(std::string_view label, const ColorSpec& fill, const ColorSpec& color, bool matchResetHeight);
    [[nodiscard]] std::unique_ptr<Flex> makeOverrideBadge();
    [[nodiscard]] std::unique_ptr<Flex> makeAdvancedBadge();
    [[nodiscard]] std::unique_ptr<Flex> makeOverrideResetActions(const std::vector<std::string>& path);
    [[nodiscard]] static bool isTemplateEnableTogglePath(const std::vector<std::string>& path);

    SettingsContentContext m_ctx;
    float m_scale;
  };

} // namespace settings
