#pragma once

#include "shell/settings/settings_registry.h"
#include "ui/controls/label.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class ConfigService;
class Button;
class Flex;
class Node;

namespace settings {

  struct SettingsStatusBannerProps {
    std::string message;
    bool error = false;
    bool scrollable = false;
    float scale = 1.0F;
    std::function<void()> onDismiss;
    Flex** out = nullptr;
    Label** messageOut = nullptr;
  };

  [[nodiscard]] std::unique_ptr<Label>
  makeLabel(std::string_view text, float fontSize, const ColorSpec& color, FontWeight fontWeight = FontWeight::Normal);
  [[nodiscard]] std::unique_ptr<Flex> makeSettingsStatusBanner(SettingsStatusBannerProps props);
  void updateSettingsStatusBanner(Flex& banner, Label& message, std::string_view text, bool error);

  // Non-dismissible callout for settings pages/sheets that need HTTP while offline_mode is on.
  // showDisableHint is false on the Security page (toggle lives there already).
  [[nodiscard]] std::unique_ptr<Flex>
  makeOfflineModeNotice(float scale, std::string message, bool showDisableHint = true);
  bool settingsSectionNeedsOfflineModeNotice(SettingsSection section);
  std::string offlineModeNoticeMessage(SettingsSection section);

  struct SettingsGroupCardProps {
    Flex& parent;
    std::string group;
    std::string title;
    float scale = 1.0F;
    std::unordered_set<std::string>& expandedGroups;
    Button* pill = nullptr;
    std::function<void(const Node&)> scrollToTop;
  };

  Flex* addSettingsGroupCard(SettingsGroupCardProps props);
  // Same card as addSettingsGroupCard without the collapsible header, for transient groupings
  // (search results) that must never start hidden and hold no expanded state.
  Flex* addSettingsCard(Flex& parent, std::string_view title, float scale);

  std::optional<std::size_t> optionIndex(const std::vector<SelectOption>& options, std::string_view value);
  std::string optionLabel(const std::vector<SelectOption>& options, std::string_view value);
  std::vector<std::string> optionLabels(const std::vector<SelectOption>& options);
  std::vector<ColorSwatchPreview> optionSwatchPreviews(const std::vector<SelectOption>& options);

  bool isMonitorOverrideSettingPath(const std::vector<std::string>& path);
  bool monitorOverrideHasExplicitValue(const Config& cfg, const std::vector<std::string>& path);
  bool settingEntryHasEffectiveOverride(const SettingEntry& entry, const ConfigService& configService);

  bool isBlankInput(std::string_view text);
  std::string formatSliderValue(double value, bool integerValue);
  std::optional<double> parseDoubleInput(std::string_view text);

  std::vector<SelectOption> sessionActionKindOptions();
  std::string
  sessionActionRowSummary(const std::vector<SelectOption>& kindOptions, const SessionPanelActionConfig& row);
  std::string sessionActionDisplayTitle(const SessionPanelActionConfig& row);

  std::string sanitizedIdleBehaviorName(std::string_view text);
  std::string uniqueIdleBehaviorName(
      std::string base, const std::vector<IdleBehaviorConfig>& rows,
      std::optional<std::size_t> ignoreIndex = std::nullopt
  );
  void normalizeIdleBehaviorNames(std::vector<IdleBehaviorConfig>& rows);
  std::string idleBehaviorRowSummary(const IdleBehaviorConfig& row);
  std::string notificationFilterRowSummary(const NotificationFilterConfig& filter);

} // namespace settings
