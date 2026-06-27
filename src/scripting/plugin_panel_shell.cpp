#include "scripting/plugin_panel_shell.h"

#include "scripting/plugin_manifest.h"

#include <algorithm>

namespace scripting {

  namespace {

    bool hasSettingKey(const PluginEntry& entry, std::string_view key) {
      return std::ranges::any_of(entry.settings, [&](const ManifestField& field) { return field.key == key; });
    }

    ManifestField makePlacementField(std::string_view entryId, std::string_view defaultValue) {
      ManifestField field;
      field.key = panelShellSettingKey(entryId, "placement");
      field.type = ManifestFieldType::Select;
      field.stringDefault = std::string(defaultValue);
      field.options = {
          {.value = "attached", .label = {}, .labelKey = {}},
          {.value = "floating", .label = {}, .labelKey = {}},
      };
      return field;
    }

    ManifestField makePositionField(std::string_view entryId, std::string_view defaultValue) {
      ManifestField field;
      field.key = panelShellSettingKey(entryId, "position");
      field.type = ManifestFieldType::Select;
      field.stringDefault = std::string(defaultValue);
      for (const std::string_view position : kPanelPositions) {
        field.options.push_back({.value = std::string(position), .label = {}, .labelKey = {}});
      }
      return field;
    }

    ManifestField makeOpenNearClickField(std::string_view entryId, bool defaultValue) {
      ManifestField field;
      field.key = panelShellSettingKey(entryId, "open_near_click");
      field.type = ManifestFieldType::Bool;
      field.boolDefault = defaultValue;
      return field;
    }

    [[nodiscard]] std::string settingString(
        const std::unordered_map<std::string, WidgetSettingValue>& settings, std::string_view key,
        std::string_view fallback
    ) {
      const auto it = settings.find(std::string(key));
      if (it == settings.end()) {
        return std::string(fallback);
      }
      if (const auto* value = std::get_if<std::string>(&it->second)) {
        return *value;
      }
      return std::string(fallback);
    }

    [[nodiscard]] bool settingBool(
        const std::unordered_map<std::string, WidgetSettingValue>& settings, std::string_view key, bool fallback
    ) {
      const auto it = settings.find(std::string(key));
      if (it == settings.end()) {
        return fallback;
      }
      if (const auto* value = std::get_if<bool>(&it->second)) {
        return *value;
      }
      if (const auto* value = std::get_if<std::int64_t>(&it->second)) {
        return *value != 0;
      }
      return fallback;
    }

  } // namespace

  std::string panelShellSettingKey(std::string_view entryId, std::string_view suffix) {
    std::string key;
    key.reserve(entryId.size() + 1 + suffix.size());
    key.append(entryId);
    key.push_back('_');
    key.append(suffix);
    return key;
  }

  void injectStandardPanelShellSettings(PluginEntry& entry) {
    if (entry.kind != PluginEntryKind::Panel) {
      return;
    }
    const std::string placementKey = panelShellSettingKey(entry.id, "placement");
    const std::string positionKey = panelShellSettingKey(entry.id, "position");
    const std::string openNearClickKey = panelShellSettingKey(entry.id, "open_near_click");
    if (!hasSettingKey(entry, placementKey)) {
      entry.settings.push_back(makePlacementField(entry.id, entry.panelPlacementDefault));
    }
    if (!hasSettingKey(entry, positionKey)) {
      entry.settings.push_back(makePositionField(entry.id, entry.panelPositionDefault));
    }
    if (!hasSettingKey(entry, openNearClickKey)) {
      entry.settings.push_back(makeOpenNearClickField(entry.id, entry.panelOpenNearClickDefault));
    }
  }

  PanelPlacement panelPlacementFromString(std::string_view value, PanelPlacement fallback) noexcept {
    if (value == "attached") {
      return PanelPlacement::Attached;
    }
    if (value == "floating") {
      return PanelPlacement::Floating;
    }
    return fallback;
  }

  bool isValidPanelPosition(std::string_view value) noexcept { return std::ranges::contains(kPanelPositions, value); }

  bool isPanelShellSettingKey(std::string_view entryId, std::string_view key) noexcept {
    return key == panelShellSettingKey(entryId, "placement")
        || key == panelShellSettingKey(entryId, "position")
        || key == panelShellSettingKey(entryId, "open_near_click");
  }

  PluginPanelShellConfig resolvePluginPanelShellConfig(
      const PluginEntry& entry, const std::unordered_map<std::string, WidgetSettingValue>& settings
  ) {
    PluginPanelShellConfig config;
    const std::string placementKey = panelShellSettingKey(entry.id, "placement");
    const std::string positionKey = panelShellSettingKey(entry.id, "position");
    const std::string openNearClickKey = panelShellSettingKey(entry.id, "open_near_click");
    config.placement = panelPlacementFromString(
        settingString(settings, placementKey, entry.panelPlacementDefault), PanelPlacement::Floating
    );
    config.position = settingString(settings, positionKey, entry.panelPositionDefault);
    if (!isValidPanelPosition(config.position)) {
      config.position = entry.panelPositionDefault;
    }
    config.openNearClick = settingBool(settings, openNearClickKey, entry.panelOpenNearClickDefault);
    return config;
  }

} // namespace scripting
