#pragma once

#include "shell/settings/settings_registry.h"
#include "ui/controls/label.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class ConfigService;

namespace settings {

  struct SettingsStatusBannerProps {
    std::string message;
    bool error = false;
    float scale = 1.0f;
    std::function<void()> onDismiss;
    Flex** out = nullptr;
    Label** messageOut = nullptr;
  };

  [[nodiscard]] std::unique_ptr<Label>
  makeLabel(std::string_view text, float fontSize, const ColorSpec& color, FontWeight fontWeight = FontWeight::Normal);
  [[nodiscard]] std::unique_ptr<Flex> makeSettingsStatusBanner(SettingsStatusBannerProps props);
  void updateSettingsStatusBanner(Flex& banner, Label& message, std::string_view text, bool error);

  [[nodiscard]] std::optional<std::size_t>
  optionIndex(const std::vector<SelectOption>& options, std::string_view value);
  [[nodiscard]] std::string optionLabel(const std::vector<SelectOption>& options, std::string_view value);
  [[nodiscard]] std::vector<std::string> optionLabels(const std::vector<SelectOption>& options);
  [[nodiscard]] std::vector<ColorSwatchPreview> optionSwatchPreviews(const std::vector<SelectOption>& options);

  [[nodiscard]] bool isMonitorOverrideSettingPath(const std::vector<std::string>& path);
  [[nodiscard]] bool monitorOverrideHasExplicitValue(const Config& cfg, const std::vector<std::string>& path);
  [[nodiscard]] bool settingEntryHasEffectiveOverride(const SettingEntry& entry, const ConfigService& configService);

  [[nodiscard]] bool isBlankInput(std::string_view text);
  [[nodiscard]] std::string formatSliderValue(double value, bool integerValue);
  [[nodiscard]] std::optional<double> parseDoubleInput(std::string_view text);

  [[nodiscard]] std::vector<SelectOption> sessionActionKindOptions();
  [[nodiscard]] std::string
  sessionActionRowSummary(const std::vector<SelectOption>& kindOptions, const SessionPanelActionConfig& row);
  [[nodiscard]] std::string sessionActionDisplayTitle(const SessionPanelActionConfig& row);

  [[nodiscard]] std::string sanitizedIdleBehaviorName(std::string_view text);
  [[nodiscard]] std::string uniqueIdleBehaviorName(
      std::string base, const std::vector<IdleBehaviorConfig>& rows,
      std::optional<std::size_t> ignoreIndex = std::nullopt
  );
  void normalizeIdleBehaviorNames(std::vector<IdleBehaviorConfig>& rows);
  [[nodiscard]] std::string idleBehaviorRowSummary(const IdleBehaviorConfig& row);
  [[nodiscard]] std::string notificationFilterRowSummary(const NotificationFilterConfig& filter);

} // namespace settings
