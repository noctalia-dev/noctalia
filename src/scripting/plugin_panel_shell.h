#pragma once

#include "config/config_types.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace scripting {

  struct PluginEntry;

  struct PluginPanelShellConfig {
    PanelPlacement placement = PanelPlacement::Floating;
    std::string position = "auto";
    bool openNearClick = false;
  };

  [[nodiscard]] std::string panelShellSettingKey(std::string_view entryId, std::string_view suffix);

  // Seeds standard placement/position/open-near-click settings on a [[panel]] entry
  // when the plugin manifest does not declare them explicitly.
  void injectStandardPanelShellSettings(PluginEntry& entry);

  [[nodiscard]] PluginPanelShellConfig resolvePluginPanelShellConfig(
      const PluginEntry& entry, const std::unordered_map<std::string, WidgetSettingValue>& settings
  );

  [[nodiscard]] PanelPlacement panelPlacementFromString(std::string_view value, PanelPlacement fallback) noexcept;
  [[nodiscard]] bool isValidPanelPosition(std::string_view value) noexcept;
  [[nodiscard]] bool isPanelShellSettingKey(std::string_view entryId, std::string_view key) noexcept;

} // namespace scripting
