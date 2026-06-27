#pragma once

#include "config/config_types.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace scripting {

  // Settings-field schema. Declared once per setting in plugin.toml and the single
  // source of truth for a setting's default (seeded into the runtime settings store).
  enum class ManifestFieldType : std::uint8_t {
    Bool,
    Int,
    Double,
    String,
    StringList,
    File,
    Folder,
    Glyph,
    Select,
    Color,
  };

  struct ManifestSelectOption {
    std::string value;
    std::string label;
    std::string labelKey;
  };

  struct ManifestVisibility {
    std::string key;
    std::vector<std::string> values;
  };

  struct ManifestField {
    std::string key;
    std::string label;
    std::string labelKey;
    std::string description;
    std::string descriptionKey;
    ManifestFieldType type = ManifestFieldType::String;

    // Typed default; the active member is selected by `type`.
    bool boolDefault = false;
    double numberDefault = 0.0;
    std::string stringDefault;
    std::vector<std::string> stringListDefault;

    std::optional<double> minValue;
    std::optional<double> maxValue;
    double step = 1.0;
    std::vector<ManifestSelectOption> options;
    std::vector<std::string> extensions;
    bool advanced = false;
    std::optional<ManifestVisibility> visibleWhen;

    // The declared default mapped to a settings value.
    [[nodiscard]] WidgetSettingValue defaultValue() const;
  };

  // Entry types a plugin may declare. P0 routes only Widget to a surface; the rest
  // are parsed and registered so the manifest format is forward-compatible.
  enum class PluginEntryKind : std::uint8_t {
    Widget,
    Panel,
    Shortcut,
    DesktopWidget,
    LauncherProvider,
    Service,
  };

  // A launcher result category, declared statically on a [[launcher_provider]]
  // entry so the launcher can render the category filter without running code.
  struct ManifestLauncherCategory {
    std::string label;
    std::string glyph;
  };

  struct PluginEntry {
    PluginEntryKind kind = PluginEntryKind::Widget;
    std::string id;    // unique within the plugin
    std::string entry; // relative .luau filename
    std::vector<ManifestField> settings;

    // Launcher-provider routing metadata (parsed only for LauncherProvider entries);
    // static so the launcher routes/filters without invoking the plugin.
    std::string launcherPrefix;
    std::string launcherGlyph;
    bool launcherGlobalSearch = false;
    std::vector<ManifestLauncherCategory> launcherCategories;
    // Wait this many ms after the last keystroke before running onQuery, so a
    // network-backed provider isn't hit on every character. 0 = no debounce.
    int launcherDebounceMs = 0;

    // Panel size in logical pixels (parsed only for Panel entries). Geometry is
    // host-owned and declared once here so the surface is sized correctly on the
    // very first open (panel.render lands async). 0 = use the host default.
    double panelWidth = 0.0;
    double panelHeight = 0.0;
    // Host-standard shell placement settings (see plugin_panel_shell.*). Parsed from
    // optional [[panel]] keys; injected settings use "{id}_placement" etc.
    std::string panelPlacementDefault = "floating";
    std::string panelPositionDefault = "auto";
    bool panelOpenNearClickDefault = false;
  };

  struct PluginManifest {
    std::string id;   // "author/plugin"
    std::string name; // mandatory display name
    std::string version;
    std::string minNoctalia; // mandatory
    std::string author;
    std::string license = "MIT";
    bool deprecated = false;
    std::vector<std::string> tags;
    std::vector<std::string> dependencies;
    std::string icon;
    std::string description;
    std::vector<PluginEntry> entries;
    // Plugin-level settings, declared once at the manifest root ([[setting]]) and
    // shared across every entry (widget, shortcut, service). Distinct from a
    // per-entry instance setting; seeded into all of the plugin's runtimes.
    std::vector<ManifestField> settings;

    [[nodiscard]] const PluginEntry* findEntry(std::string_view entryId) const;
  };

  // The TOML array-table name for each entry kind (e.g. "widget" -> [[widget]]).
  [[nodiscard]] std::string_view pluginEntryTableName(PluginEntryKind kind);

  // Build the runtime settings for an instance: every declared field seeded with
  // its manifest default, then overlaid by the instance's configured values.
  // Only declared keys are emitted, so a `getConfig` of an undeclared key stays a
  // loud miss rather than silently resolving from a stray override.
  [[nodiscard]] std::unordered_map<std::string, WidgetSettingValue>
  seedEntrySettings(const PluginEntry& entry, const std::unordered_map<std::string, WidgetSettingValue>& overrides);

  // Overlay the plugin-level settings (manifest defaults, then `pluginOverrides`)
  // onto an already-seeded entry settings map. A key already present (an entry-level
  // setting) is left untouched — entry-level wins over plugin-level.
  void mergePluginSettings(
      const PluginManifest& manifest, const std::unordered_map<std::string, WidgetSettingValue>& pluginOverrides,
      std::unordered_map<std::string, WidgetSettingValue>& seeded
  );

  // Settings-map equality with int/double coercion (matches config override
  // comparison), so reseeding a service from an unchanged config is a no-op.
  [[nodiscard]] bool settingsEqual(
      const std::unordered_map<std::string, WidgetSettingValue>& a,
      const std::unordered_map<std::string, WidgetSettingValue>& b
  );

  // Parse a plugin.toml. Returns nullopt and sets `error` on a hard failure:
  // unreadable file, TOML parse error, or a missing mandatory `id` / `name` / `min_noctalia`.
  // Entry ids are validated for uniqueness within the plugin.
  [[nodiscard]] std::optional<PluginManifest>
  parsePluginManifest(const std::filesystem::path& manifestPath, std::string* error);

} // namespace scripting
