#include "shell/settings/settings_registry.h"

#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "core/log.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/settings/settings_registry_common.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace settings {
  namespace {

    constexpr std::array<SettingsSectionDescriptor, 25> kSettingsSections{{
        {SettingsSection::Appearance, "appearance", "adjustments-horizontal"},
        {SettingsSection::Wallpaper, "wallpaper", "paint"},
        {SettingsSection::Templates, "templates", "color-swatch"},
        {SettingsSection::Desktop, "desktop", "layout-board"},
        {SettingsSection::LockScreen, "lock-screen", "lock"},
        {SettingsSection::Dock, "dock", "layout-bottombar-inactive"},
        {SettingsSection::Panels, "panels", "layout-bottombar"},
        {SettingsSection::Launcher, "launcher", "rocket"},
        {SettingsSection::ControlCenter, "control-center", "adjustments"},
        {SettingsSection::Notifications, "notifications", "bell"},
        {SettingsSection::Osd, "osd", "message-circle"},
        {SettingsSection::Screenshot, "screenshot", "screenshot"},
        {SettingsSection::Shell, "shell", "app-window"},
        {SettingsSection::Keybinds, "keybinds", "keyboard"},
        {SettingsSection::Security, "security", "shield-lock"},
        {SettingsSection::System, "system", "activity-heartbeat"},
        {SettingsSection::Services, "services", "stack-2"},
        {SettingsSection::Location, "location", "map-pin"},
        {SettingsSection::Calendar, "calendar", "calendar"},
        {SettingsSection::Power, "power", "bolt"},
        {SettingsSection::Hooks, "hooks", "link"},
        {SettingsSection::Niri, "niri", "niri"},
        {SettingsSection::Umbriel, "umbriel", "umbriel"},
        {SettingsSection::Bar, "bar", "crop-3-2", false},
        {SettingsSection::Plugins, "plugins", "puzzle", true, true},
    }};

    const SettingsSectionDescriptor& descriptorFor(SettingsSection section) {
      const auto* it = std::ranges::find(kSettingsSections, section, &SettingsSectionDescriptor::section);
      if (it == kSettingsSections.end()) {
        std::abort();
      }
      return *it;
    }

  } // namespace

  const BarConfig* findBar(const Config& cfg, std::string_view name) {
    for (const auto& bar : cfg.bars) {
      if (bar.name == name) {
        return &bar;
      }
    }
    return nullptr;
  }

  const BarMonitorOverride* findMonitorOverride(const BarConfig& bar, std::string_view match) {
    for (const auto& ovr : bar.monitorOverrides) {
      if (ovr.match == match) {
        return &ovr;
      }
    }
    return nullptr;
  }

  const DockMonitorOverride* findDockMonitorOverride(const DockConfig& dock, std::string_view tableName) {
    const auto it = std::ranges::find(dock.monitorOverrides, tableName, &DockMonitorOverride::tableName);
    return it != dock.monitorOverrides.end() ? &*it : nullptr;
  }

  std::vector<std::string> barNames(const Config& cfg) {
    std::vector<std::string> names;
    names.reserve(cfg.bars.size());
    for (const auto& bar : cfg.bars) {
      names.push_back(bar.name);
    }
    return names;
  }

  std::string normalizedSettingQuery(std::string_view query) { return StringUtils::normalizeForSearch(query); }

  bool matchesNormalizedSettingQuery(const SettingEntry& entry, std::string_view normalizedQuery) {
    if (normalizedQuery.empty()) {
      return true;
    }
    return entry.searchText.contains(normalizedQuery);
  }

  bool matchesSettingQuery(const SettingEntry& entry, std::string_view query) {
    return matchesNormalizedSettingQuery(entry, normalizedSettingQuery(query));
  }

  bool isBarMonitorOverrideSettingPath(const std::vector<std::string>& path) {
    return path.size() >= 5 && path[0] == "bar" && path[2] == "monitor";
  }

  bool isDockMonitorOverrideSettingPath(const std::vector<std::string>& path) {
    return path.size() >= 4 && path[0] == "dock" && path[1] == "monitor";
  }

  bool settingEntryMatchesBarNavigation(
      const SettingEntry& entry, std::string_view selectedBarName, std::string_view selectedMonitorOverride
  ) {
    if (entry.section != SettingsSection::Bar || entry.path.size() < 2 || entry.path[0] != "bar") {
      return false;
    }
    if (selectedBarName.empty() || entry.path[1] != selectedBarName) {
      return false;
    }
    const bool monitorEntry = isBarMonitorOverrideSettingPath(entry.path);
    if (selectedMonitorOverride.empty()) {
      return !monitorEntry;
    }
    return monitorEntry && entry.path[3] == selectedMonitorOverride;
  }

  bool settingEntryMatchesDockNavigation(const SettingEntry& entry, std::string_view selectedMonitorOverride) {
    if (entry.section != SettingsSection::Dock || entry.path.empty() || entry.path[0] != "dock") {
      return false;
    }
    const bool monitorEntry = isDockMonitorOverrideSettingPath(entry.path);
    if (selectedMonitorOverride.empty()) {
      return !monitorEntry;
    }
    return monitorEntry && entry.path[2] == selectedMonitorOverride;
  }

  std::string barSettingContentSectionKey(const SettingEntry& entry) {
    if (entry.section == SettingsSection::Dock) {
      std::string key = "dock";
      if (isDockMonitorOverrideSettingPath(entry.path)) {
        key += ":monitor:" + entry.path[2];
      }
      return key;
    }
    if (entry.section != SettingsSection::Bar || entry.path.size() < 2) {
      return std::string(settingsSectionId(entry.section));
    }
    std::string key = "bar:" + entry.path[1];
    if (isBarMonitorOverrideSettingPath(entry.path)) {
      key += ":monitor:" + entry.path[3];
    }
    return key;
  }

  std::span<const SettingsSectionDescriptor> settingsSectionDescriptors() { return kSettingsSections; }

  std::string_view settingsSectionId(SettingsSection section) { return descriptorFor(section).id; }

  std::string settingsSectionLabelKey(SettingsSection section) {
    return "settings.navigation.sections." + std::string(settingsSectionId(section));
  }

  std::string_view sectionGlyph(SettingsSection section) { return descriptorFor(section).glyph; }

  std::optional<SettingsSection> settingsSectionFromId(std::string_view id) {
    const auto* it = std::ranges::find(kSettingsSections, id, &SettingsSectionDescriptor::id);
    if (it == kSettingsSections.end()) {
      return std::nullopt;
    }
    return it->section;
  }

  std::vector<SettingEntry> buildSettingsRegistry(
      const Config& cfg, const BarConfig* selectedBar, const BarMonitorOverride* selectedMonitorOverride,
      const RegistryEnvironment& env
  ) {
    (void)selectedBar;
    (void)selectedMonitorOverride;
    std::vector<SettingEntry> entries;
    registry::addAppearanceSettings(entries, cfg, env);
    registry::addWallpaperSettings(entries, cfg, env);
    registry::addTemplateSettings(entries, cfg);
    registry::addDockSettings(entries, cfg, env);
    registry::addPanelSettings(entries, cfg);
    registry::addLauncherSettings(entries, cfg);
    registry::addClipboardAndWallpaperPanelSettings(entries, cfg);
    registry::addControlCenterSettings(entries, cfg);
    registry::addDesktopSettings(entries, cfg);
    registry::addSecuritySettings(entries, cfg);
    registry::addLockScreenSettings(entries, cfg, env);
    registry::addShellSettings(entries, cfg, env);
    registry::addScreenshotSettings(entries, cfg);
    registry::addWindowSwitcherSettings(entries, cfg);
    registry::addOsdSettings(entries, cfg, env);
    registry::addKeybindSettings(entries, cfg);
    registry::addNiriSettings(entries, cfg, env);
    registry::addUmbrielSettings(entries, cfg, env);
    registry::addSystemSettings(entries, cfg, env);
    registry::addLocationSettings(entries, cfg, env);
    registry::addCalendarSettings(entries, cfg);
    registry::addServiceSettings(entries, cfg, env);
    registry::addPowerSettings(entries, cfg);
    registry::addIdleSettings(entries, cfg);
    registry::addHookSettings(entries, cfg);
    registry::addNotificationSettings(entries, cfg, env);
    registry::addBarSettings(entries, cfg, env);
    registry::addBarMonitorOverrideSettings(entries, cfg, env);

    // Integrity guard: every override path must resolve to a real schema key, else
    // the entry silently reads/writes a dead override. Build-determined, so checked
    // once per process; warn-only. (Visibility predicates are compiler-checked.)
    {
      static bool verified = false;
      if (!verified) {
        verified = true;
        const Logger log("settings");
        const auto verify = [&](const std::vector<std::string>& path, std::string_view what) {
          if (!path.empty() && !noctalia::config::schema::isKnownConfigPath(path)) {
            std::string dotted;
            for (const auto& seg : path) {
              dotted += (dotted.empty() ? "" : ".") + seg;
            }
            log.warn("settings registry {} path does not resolve to a schema key: {}", what, dotted);
          }
        };
        for (const auto& entry : entries) {
          if (!std::holds_alternative<ButtonSetting>(entry.control)) {
            verify(entry.path, "override");
          }
        }
      }
    }

    return entries;
  }

} // namespace settings
