#include "config/config_service.h"
#include "core/key_chord.h"
#include "core/log.h"
#include "shell/settings/widget_settings_registry.h"
#include "theme/scheme.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
  constexpr Logger kLog("config");
  constexpr double kConfigFloatEpsilon = 1.0e-5;

  std::string overrideCacheKey(const std::vector<std::string>& path) {
    std::string key;
    for (const auto& part : path) {
      if (!key.empty()) {
        key.push_back('.');
      }
      key += part;
    }
    return key;
  }

  bool nearlyEqual(double a, double b) noexcept { return std::abs(a - b) <= kConfigFloatEpsilon; }

  bool colorEqual(const Color& a, const Color& b) noexcept {
    return nearlyEqual(a.r, b.r) && nearlyEqual(a.g, b.g) && nearlyEqual(a.b, b.b) && nearlyEqual(a.a, b.a);
  }

  bool colorSpecEqual(const ColorSpec& a, const ColorSpec& b) noexcept {
    return a.role == b.role && colorEqual(a.fixed, b.fixed) && nearlyEqual(a.alpha, b.alpha);
  }

  bool optionalDoubleEqual(const std::optional<double>& a, const std::optional<double>& b) noexcept {
    if (a.has_value() != b.has_value()) {
      return false;
    }
    return !a.has_value() || nearlyEqual(*a, *b);
  }

  bool optionalColorSpecEqual(const std::optional<ColorSpec>& a, const std::optional<ColorSpec>& b) noexcept {
    if (a.has_value() != b.has_value()) {
      return false;
    }
    return !a.has_value() || colorSpecEqual(*a, *b);
  }

  template <typename T, typename Equal>
  bool vectorEqual(const std::vector<T>& a, const std::vector<T>& b, Equal equal) {
    if (a.size() != b.size()) {
      return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (!equal(a[i], b[i])) {
        return false;
      }
    }
    return true;
  }

  std::optional<double> numericWidgetSetting(const WidgetSettingValue& value) {
    if (const auto* i = std::get_if<std::int64_t>(&value)) {
      return static_cast<double>(*i);
    }
    if (const auto* d = std::get_if<double>(&value)) {
      return *d;
    }
    return std::nullopt;
  }

  bool widgetSettingEqual(const WidgetSettingValue& a, const WidgetSettingValue& b) {
    const auto aNum = numericWidgetSetting(a);
    const auto bNum = numericWidgetSetting(b);
    if (aNum.has_value() || bNum.has_value()) {
      return aNum.has_value() && bNum.has_value() && nearlyEqual(*aNum, *bNum);
    }
    if (a.index() != b.index()) {
      return false;
    }
    return std::visit(
        [&](const auto& av) {
          using T = std::decay_t<decltype(av)>;
          const auto* bv = std::get_if<T>(&b);
          return bv != nullptr && av == *bv;
        },
        a
    );
  }

  bool widgetSettingsEqual(
      const std::unordered_map<std::string, WidgetSettingValue>& a,
      const std::unordered_map<std::string, WidgetSettingValue>& b
  ) {
    if (a.size() != b.size()) {
      return false;
    }
    for (const auto& [key, value] : a) {
      const auto it = b.find(key);
      if (it == b.end() || !widgetSettingEqual(value, it->second)) {
        return false;
      }
    }
    return true;
  }

  bool desktopWidgetEqual(const DesktopWidgetState& a, const DesktopWidgetState& b) {
    return a.id == b.id && a.type == b.type && a.outputName == b.outputName && nearlyEqual(a.cx, b.cx) &&
           nearlyEqual(a.cy, b.cy) && nearlyEqual(a.scale, b.scale) && nearlyEqual(a.rotationRad, b.rotationRad) &&
           a.enabled == b.enabled && widgetSettingsEqual(a.settings, b.settings);
  }

  bool desktopWidgetsConfigEqual(const DesktopWidgetsConfig& a, const DesktopWidgetsConfig& b) {
    return a.enabled == b.enabled && a.schemaVersion == b.schemaVersion && a.grid.visible == b.grid.visible &&
           a.grid.cellSize == b.grid.cellSize && a.grid.majorInterval == b.grid.majorInterval &&
           vectorEqual(a.widgets, b.widgets, desktopWidgetEqual);
  }

  bool barBaseConfigEqual(const BarConfig& a, const BarConfig& b) {
    return a.name == b.name && a.position == b.position && a.enabled == b.enabled && a.autoHide == b.autoHide &&
           a.reserveSpace == b.reserveSpace && a.thickness == b.thickness &&
           nearlyEqual(a.backgroundOpacity, b.backgroundOpacity) && colorSpecEqual(a.border, b.border) &&
           nearlyEqual(a.borderWidth, b.borderWidth) && a.radius == b.radius && a.radiusTopLeft == b.radiusTopLeft &&
           a.radiusTopRight == b.radiusTopRight && a.radiusBottomLeft == b.radiusBottomLeft &&
           a.radiusBottomRight == b.radiusBottomRight && a.marginEnds == b.marginEnds && a.marginEdge == b.marginEdge &&
           a.padding == b.padding && a.widgetSpacing == b.widgetSpacing && a.shadow == b.shadow &&
           a.contactShadow == b.contactShadow && nearlyEqual(a.scale, b.scale) && a.fontWeight == b.fontWeight &&
           a.startWidgets == b.startWidgets && a.centerWidgets == b.centerWidgets && a.endWidgets == b.endWidgets &&
           a.widgetCapsuleDefault == b.widgetCapsuleDefault &&
           colorSpecEqual(a.widgetCapsuleFill, b.widgetCapsuleFill) &&
           optionalColorSpecEqual(a.widgetCapsuleForeground, b.widgetCapsuleForeground) &&
           optionalColorSpecEqual(a.widgetColor, b.widgetColor) && a.widgetCapsuleGroups == b.widgetCapsuleGroups &&
           nearlyEqual(a.widgetCapsulePadding, b.widgetCapsulePadding) &&
           optionalDoubleEqual(a.widgetCapsuleRadius, b.widgetCapsuleRadius) &&
           nearlyEqual(a.widgetCapsuleOpacity, b.widgetCapsuleOpacity) &&
           a.widgetCapsuleBorderSpecified == b.widgetCapsuleBorderSpecified &&
           optionalColorSpecEqual(a.widgetCapsuleBorder, b.widgetCapsuleBorder);
  }

  BarConfig applyMonitorOverrideForComparison(const BarConfig& base, const BarMonitorOverride& ovr) {
    BarConfig resolved = base;
    resolved.monitorOverrides.clear();
    if (ovr.enabled) {
      resolved.enabled = *ovr.enabled;
    }
    if (ovr.autoHide) {
      resolved.autoHide = *ovr.autoHide;
    }
    if (ovr.reserveSpace) {
      resolved.reserveSpace = *ovr.reserveSpace;
    }
    if (ovr.thickness) {
      resolved.thickness = *ovr.thickness;
    }
    if (ovr.backgroundOpacity) {
      resolved.backgroundOpacity = *ovr.backgroundOpacity;
    }
    if (ovr.border) {
      resolved.border = *ovr.border;
    }
    if (ovr.borderWidth) {
      resolved.borderWidth = *ovr.borderWidth;
    }
    if (ovr.radius) {
      resolved.radius = *ovr.radius;
      resolved.radiusTopLeft = *ovr.radius;
      resolved.radiusTopRight = *ovr.radius;
      resolved.radiusBottomLeft = *ovr.radius;
      resolved.radiusBottomRight = *ovr.radius;
    }
    if (ovr.radiusTopLeft) {
      resolved.radiusTopLeft = *ovr.radiusTopLeft;
    }
    if (ovr.radiusTopRight) {
      resolved.radiusTopRight = *ovr.radiusTopRight;
    }
    if (ovr.radiusBottomLeft) {
      resolved.radiusBottomLeft = *ovr.radiusBottomLeft;
    }
    if (ovr.radiusBottomRight) {
      resolved.radiusBottomRight = *ovr.radiusBottomRight;
    }
    if (ovr.marginEnds) {
      resolved.marginEnds = *ovr.marginEnds;
    }
    if (ovr.marginEdge) {
      resolved.marginEdge = *ovr.marginEdge;
    }
    if (ovr.padding) {
      resolved.padding = *ovr.padding;
    }
    if (ovr.widgetSpacing) {
      resolved.widgetSpacing = *ovr.widgetSpacing;
    }
    if (ovr.shadow) {
      resolved.shadow = *ovr.shadow;
    }
    if (ovr.contactShadow) {
      resolved.contactShadow = *ovr.contactShadow;
    }
    if (ovr.startWidgets) {
      resolved.startWidgets = *ovr.startWidgets;
    }
    if (ovr.centerWidgets) {
      resolved.centerWidgets = *ovr.centerWidgets;
    }
    if (ovr.endWidgets) {
      resolved.endWidgets = *ovr.endWidgets;
    }
    if (ovr.scale) {
      resolved.scale = *ovr.scale;
    }
    if (ovr.widgetCapsuleDefault) {
      resolved.widgetCapsuleDefault = *ovr.widgetCapsuleDefault;
    }
    if (ovr.widgetCapsuleFill) {
      resolved.widgetCapsuleFill = *ovr.widgetCapsuleFill;
    }
    if (ovr.widgetCapsuleBorderSpecified) {
      resolved.widgetCapsuleBorderSpecified = true;
      resolved.widgetCapsuleBorder = ovr.widgetCapsuleBorder;
    }
    if (ovr.widgetCapsuleForeground) {
      resolved.widgetCapsuleForeground = *ovr.widgetCapsuleForeground;
    }
    if (ovr.widgetColor) {
      resolved.widgetColor = *ovr.widgetColor;
    }
    if (ovr.widgetCapsuleGroups) {
      resolved.widgetCapsuleGroups = *ovr.widgetCapsuleGroups;
    }
    if (ovr.widgetCapsulePadding) {
      resolved.widgetCapsulePadding = std::clamp(static_cast<float>(*ovr.widgetCapsulePadding), 0.0f, 48.0f);
    }
    if (ovr.widgetCapsuleRadius.has_value()) {
      resolved.widgetCapsuleRadius = std::clamp(*ovr.widgetCapsuleRadius, 0.0, 80.0);
    }
    if (ovr.widgetCapsuleOpacity) {
      resolved.widgetCapsuleOpacity = std::clamp(static_cast<float>(*ovr.widgetCapsuleOpacity), 0.0f, 1.0f);
    }
    return resolved;
  }

  bool barMonitorOverrideEqual(const BarConfig& base, const BarMonitorOverride& a, const BarMonitorOverride& b) {
    return a.match == b.match &&
           barBaseConfigEqual(applyMonitorOverrideForComparison(base, a), applyMonitorOverrideForComparison(base, b));
  }

  bool barConfigEqual(const BarConfig& a, const BarConfig& b) {
    return barBaseConfigEqual(a, b) && vectorEqual(
                                           a.monitorOverrides, b.monitorOverrides,
                                           [&a](const BarMonitorOverride& lhs, const BarMonitorOverride& rhs) {
                                             return barMonitorOverrideEqual(a, lhs, rhs);
                                           }
                                       );
  }

  bool widgetConfigEqual(const WidgetConfig& a, const WidgetConfig& b) {
    return a.type == b.type && widgetSettingsEqual(a.settings, b.settings);
  }

  bool widgetMapEqual(
      const std::unordered_map<std::string, WidgetConfig>& a, const std::unordered_map<std::string, WidgetConfig>& b
  ) {
    if (a.size() != b.size()) {
      return false;
    }
    for (const auto& [key, value] : a) {
      const auto it = b.find(key);
      if (it == b.end() || !widgetConfigEqual(value, it->second)) {
        return false;
      }
    }
    return true;
  }

  bool isWidgetSettingOverridePath(const std::vector<std::string>& path) {
    return path.size() == 3 && path[0] == "widget";
  }

  std::string widgetTypeForOverridePath(const toml::table& overrides, std::string_view widgetName) {
    if (const auto* widgetTable = overrides.get_as<toml::table>("widget")) {
      if (const auto* entry = widgetTable->get_as<toml::table>(widgetName)) {
        if (const auto type = (*entry)["type"].value<std::string>()) {
          return *type;
        }
      }
    }
    return std::string(widgetName);
  }

  bool wallpaperMonitorOverrideEqual(const WallpaperMonitorOverride& a, const WallpaperMonitorOverride& b) {
    return a.match == b.match && a.enabled == b.enabled && optionalColorSpecEqual(a.fillColor, b.fillColor) &&
           a.directory == b.directory && a.directoryLight == b.directoryLight && a.directoryDark == b.directoryDark;
  }

  bool wallpaperConfigEqual(const WallpaperConfig& a, const WallpaperConfig& b) {
    return a.enabled == b.enabled && a.fillMode == b.fillMode && optionalColorSpecEqual(a.fillColor, b.fillColor) &&
           a.transitions == b.transitions && nearlyEqual(a.transitionDurationMs, b.transitionDurationMs) &&
           nearlyEqual(a.edgeSmoothness, b.edgeSmoothness) && a.directory == b.directory &&
           a.directoryLight == b.directoryLight && a.directoryDark == b.directoryDark &&
           a.perMonitorDirectories == b.perMonitorDirectories && a.automation.enabled == b.automation.enabled &&
           a.automation.intervalMinutes == b.automation.intervalMinutes && a.automation.order == b.automation.order &&
           a.automation.recursive == b.automation.recursive &&
           vectorEqual(a.monitorOverrides, b.monitorOverrides, wallpaperMonitorOverrideEqual);
  }

  bool dockConfigEqual(const DockConfig& a, const DockConfig& b) {
    return a.enabled == b.enabled && a.position == b.position && a.activeMonitorOnly == b.activeMonitorOnly &&
           a.iconSize == b.iconSize && a.padding == b.padding && a.itemSpacing == b.itemSpacing &&
           nearlyEqual(a.backgroundOpacity, b.backgroundOpacity) && a.radius == b.radius &&
           a.radiusTopLeft == b.radiusTopLeft && a.radiusTopRight == b.radiusTopRight &&
           a.radiusBottomLeft == b.radiusBottomLeft && a.radiusBottomRight == b.radiusBottomRight &&
           a.marginEnds == b.marginEnds && a.marginEdge == b.marginEdge && a.shadow == b.shadow &&
           a.showRunning == b.showRunning && a.autoHide == b.autoHide && a.reserveSpace == b.reserveSpace &&
           nearlyEqual(a.activeScale, b.activeScale) && nearlyEqual(a.inactiveScale, b.inactiveScale) &&
           nearlyEqual(a.activeOpacity, b.activeOpacity) && nearlyEqual(a.inactiveOpacity, b.inactiveOpacity) &&
           a.showDots == b.showDots && a.showInstanceCount == b.showInstanceCount &&
           a.launcherPosition == b.launcherPosition && a.launcherIcon == b.launcherIcon && a.pinned == b.pinned &&
           a.monitors == b.monitors;
  }

  bool shellConfigEqual(const ShellConfig& a, const ShellConfig& b) {
    return nearlyEqual(a.uiScale, b.uiScale) && nearlyEqual(a.cornerRadiusScale, b.cornerRadiusScale) &&
           a.fontFamily == b.fontFamily && a.lang == b.lang && a.timeFormat == b.timeFormat &&
           a.dateFormat == b.dateFormat && a.offlineMode == b.offlineMode && a.telemetryEnabled == b.telemetryEnabled &&
           a.niriOverviewTypeToLaunchEnabled == b.niriOverviewTypeToLaunchEnabled && a.polkitAgent == b.polkitAgent &&
           a.passwordMaskStyle == b.passwordMaskStyle && a.animation.enabled == b.animation.enabled &&
           nearlyEqual(a.animation.speed, b.animation.speed) && a.avatarPath == b.avatarPath &&
           a.settingsShowAdvanced == b.settingsShowAdvanced &&
           a.middleClickOpensWidgetSettings == b.middleClickOpensWidgetSettings && a.showLocation == b.showLocation &&
           a.launchAppsAsSystemdServices == b.launchAppsAsSystemdServices && a.clipboardEnabled == b.clipboardEnabled &&
           a.clipboardHistoryMaxEntries == b.clipboardHistoryMaxEntries && a.screenTimeEnabled == b.screenTimeEnabled &&
           a.clipboardAutoPaste == b.clipboardAutoPaste &&
           a.clipboardImageActionCommand == b.clipboardImageActionCommand && a.shadow.blur == b.shadow.blur &&
           a.shadow.offsetX == b.shadow.offsetX && a.shadow.offsetY == b.shadow.offsetY &&
           nearlyEqual(a.shadow.alpha, b.shadow.alpha) && a.panel.backgroundBlur == b.panel.backgroundBlur &&
           a.panel.borders == b.panel.borders && a.panel.shadow == b.panel.shadow &&
           a.panel.transparencyMode == b.panel.transparencyMode &&
           a.panel.launcherPlacement == b.panel.launcherPlacement &&
           a.panel.clipboardPlacement == b.panel.clipboardPlacement &&
           a.panel.controlCenterPlacement == b.panel.controlCenterPlacement &&
           a.panel.wallpaperPlacement == b.panel.wallpaperPlacement &&
           a.panel.sessionPlacement == b.panel.sessionPlacement &&
           a.panel.openNearClickControlCenter == b.panel.openNearClickControlCenter &&
           a.panel.openNearClickLauncher == b.panel.openNearClickLauncher &&
           a.panel.openNearClickClipboard == b.panel.openNearClickClipboard &&
           a.panel.openNearClickWallpaper == b.panel.openNearClickWallpaper &&
           a.panel.openNearClickSession == b.panel.openNearClickSession &&
           a.panel.launcherCategories == b.panel.launcherCategories &&
           a.screenCorners.enabled == b.screenCorners.enabled && a.screenCorners.size == b.screenCorners.size &&
           a.mpris.blacklist == b.mpris.blacklist && a.session.actions == b.session.actions;
  }

  bool notificationConfigEqual(const NotificationConfig& a, const NotificationConfig& b) {
    return a.enableDaemon == b.enableDaemon && a.position == b.position && a.layer == b.layer &&
           nearlyEqual(a.scale, b.scale) && nearlyEqual(a.backgroundOpacity, b.backgroundOpacity) &&
           a.offsetX == b.offsetX && a.offsetY == b.offsetY && a.monitors == b.monitors;
  }

  bool audioConfigEqual(const AudioConfig& a, const AudioConfig& b) {
    return a.enableOverdrive == b.enableOverdrive && a.enableSounds == b.enableSounds &&
           nearlyEqual(a.soundVolume, b.soundVolume) && a.volumeChangeSound == b.volumeChangeSound &&
           a.notificationSound == b.notificationSound;
  }

  bool nightLightConfigEqual(const NightLightConfig& a, const NightLightConfig& b) {
    return a.enabled == b.enabled && a.force == b.force && a.useWeatherLocation == b.useWeatherLocation &&
           a.startTime == b.startTime && a.stopTime == b.stopTime && optionalDoubleEqual(a.latitude, b.latitude) &&
           optionalDoubleEqual(a.longitude, b.longitude) && a.dayTemperature == b.dayTemperature &&
           a.nightTemperature == b.nightTemperature;
  }

  bool idleConfigEqual(const IdleConfig& a, const IdleConfig& b) {
    return nearlyEqual(a.preActionFadeSeconds, b.preActionFadeSeconds) &&
           vectorEqual(a.behaviors, b.behaviors, [](const IdleBehaviorConfig& lhs, const IdleBehaviorConfig& rhs) {
             return lhs.name == rhs.name && lhs.enabled == rhs.enabled && lhs.timeoutSeconds == rhs.timeoutSeconds &&
                    lhs.action == rhs.action && lhs.command == rhs.command && lhs.resumeCommand == rhs.resumeCommand;
           });
  }

  bool themeConfigEqual(const ThemeConfig& a, const ThemeConfig& b) {
    return a.source == b.source && a.builtinPalette == b.builtinPalette && a.communityPalette == b.communityPalette &&
           a.customPalette == b.customPalette && a.wallpaperScheme == b.wallpaperScheme && a.mode == b.mode &&
           a.templates.enableBuiltinTemplates == b.templates.enableBuiltinTemplates &&
           a.templates.builtinIds == b.templates.builtinIds &&
           a.templates.enableCommunityTemplates == b.templates.enableCommunityTemplates &&
           a.templates.communityIds == b.templates.communityIds &&
           a.templates.customColors == b.templates.customColors &&
           a.templates.userTemplates == b.templates.userTemplates;
  }

  bool configEqual(const Config& a, const Config& b) {
    return vectorEqual(a.bars, b.bars, barConfigEqual) && widgetMapEqual(a.widgets, b.widgets) &&
           wallpaperConfigEqual(a.wallpaper, b.wallpaper) && a.backdrop.enabled == b.backdrop.enabled &&
           nearlyEqual(a.backdrop.blurIntensity, b.backdrop.blurIntensity) &&
           nearlyEqual(a.backdrop.tintIntensity, b.backdrop.tintIntensity) && dockConfigEqual(a.dock, b.dock) &&
           desktopWidgetsConfigEqual(a.desktopWidgets, b.desktopWidgets) && shellConfigEqual(a.shell, b.shell) &&
           a.osd.position == b.osd.position && a.osd.orientation == b.osd.orientation &&
           nearlyEqual(a.osd.scale, b.osd.scale) && a.osd.lockKeys == b.osd.lockKeys &&
           a.osd.keyboardLayout == b.osd.keyboardLayout && notificationConfigEqual(a.notification, b.notification) &&
           a.weather.enabled == b.weather.enabled && a.weather.autoLocate == b.weather.autoLocate &&
           a.weather.effects == b.weather.effects && a.weather.address == b.weather.address &&
           a.weather.refreshMinutes == b.weather.refreshMinutes && a.weather.unit == b.weather.unit &&
           a.system.monitor.enabled == b.system.monitor.enabled &&
           a.system.monitor.cpuPollSeconds == b.system.monitor.cpuPollSeconds &&
           a.system.monitor.gpuPollSeconds == b.system.monitor.gpuPollSeconds &&
           a.system.monitor.memoryPollSeconds == b.system.monitor.memoryPollSeconds &&
           a.system.monitor.networkPollSeconds == b.system.monitor.networkPollSeconds &&
           a.system.monitor.diskPollSeconds == b.system.monitor.diskPollSeconds && audioConfigEqual(a.audio, b.audio) &&
           a.brightness == b.brightness && a.keybinds.validate == b.keybinds.validate &&
           a.keybinds.cancel == b.keybinds.cancel && a.keybinds.left == b.keybinds.left &&
           a.keybinds.right == b.keybinds.right && a.keybinds.up == b.keybinds.up &&
           a.keybinds.down == b.keybinds.down && nightLightConfigEqual(a.nightlight, b.nightlight) &&
           idleConfigEqual(a.idle, b.idle) && a.hooks == b.hooks && themeConfigEqual(a.theme, b.theme) &&
           a.controlCenter == b.controlCenter;
  }

  toml::table* ensureTable(toml::table& parent, std::string_view key) {
    if (auto* existing = parent.get_as<toml::table>(key)) {
      return existing;
    }
    auto [it, _] = parent.insert_or_assign(key, toml::table{});
    return it->second.as_table();
  }

  void insertWidgetSetting(toml::table& table, const std::string& key, const WidgetSettingValue& value) {
    std::visit(
        [&](const auto& concrete) {
          using T = std::decay_t<decltype(concrete)>;
          if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            toml::array array;
            for (const auto& item : concrete) {
              array.push_back(item);
            }
            table.insert_or_assign(key, std::move(array));
          } else {
            table.insert_or_assign(key, concrete);
          }
        },
        value
    );
  }

  toml::table desktopWidgetTable(const DesktopWidgetState& widget) {
    toml::table widgetTable;
    widgetTable.insert_or_assign("type", widget.type);
    widgetTable.insert_or_assign("output", widget.outputName);
    widgetTable.insert_or_assign("cx", static_cast<double>(widget.cx));
    widgetTable.insert_or_assign("cy", static_cast<double>(widget.cy));
    widgetTable.insert_or_assign("scale", static_cast<double>(widget.scale));
    widgetTable.insert_or_assign("rotation", static_cast<double>(widget.rotationRad));
    if (!widget.enabled) {
      widgetTable.insert_or_assign("enabled", false);
    }
    if (!widget.settings.empty()) {
      toml::table settingsTable;
      for (const auto& [key, value] : widget.settings) {
        insertWidgetSetting(settingsTable, key, value);
      }
      widgetTable.insert_or_assign("settings", std::move(settingsTable));
    }
    return widgetTable;
  }

  void insertOverrideValue(toml::table& table, std::string_view key, const ConfigOverrideValue& value) {
    std::visit(
        [&](const auto& concrete) {
          using T = std::decay_t<decltype(concrete)>;
          if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            toml::array array;
            for (const auto& item : concrete) {
              array.push_back(item);
            }
            table.insert_or_assign(key, std::move(array));
          } else if constexpr (std::is_same_v<T, std::vector<ShortcutConfig>>) {
            toml::array array;
            for (const auto& item : concrete) {
              if (item.type.empty()) {
                continue;
              }
              toml::table shortcut;
              shortcut.insert_or_assign("type", item.type);
              array.push_back(std::move(shortcut));
            }
            table.insert_or_assign(key, std::move(array));
          } else if constexpr (std::is_same_v<T, std::vector<SessionPanelActionConfig>>) {
            toml::array array;
            for (const auto& item : concrete) {
              if (item.action.empty()) {
                continue;
              }
              toml::table row;
              row.insert_or_assign("action", item.action);
              row.insert_or_assign("enabled", item.enabled);
              if (item.command.has_value() && !item.command->empty()) {
                row.insert_or_assign("command", *item.command);
              }
              if (item.label.has_value() && !item.label->empty()) {
                row.insert_or_assign("label", *item.label);
              }
              if (item.glyph.has_value() && !item.glyph->empty()) {
                row.insert_or_assign("glyph", *item.glyph);
              }
              row.insert_or_assign("variant", std::string(enumToKey(kSessionActionButtonVariants, item.variant)));
              if (item.shortcut.has_value()) {
                row.insert_or_assign("shortcut", keyChordToString(*item.shortcut));
              }
              array.push_back(std::move(row));
            }
            table.insert_or_assign(key, std::move(array));
          } else if constexpr (std::is_same_v<T, std::vector<IdleBehaviorConfig>>) {
            toml::table behaviorTable;
            toml::array behaviorOrder;
            for (const auto& item : concrete) {
              if (item.name.empty()) {
                continue;
              }
              toml::table row;
              row.insert_or_assign("enabled", item.enabled);
              row.insert_or_assign("timeout", static_cast<std::int64_t>(item.timeoutSeconds));
              if (!item.action.empty()) {
                row.insert_or_assign("action", item.action);
              }
              if (!item.command.empty()) {
                row.insert_or_assign("command", item.command);
              }
              if (!item.resumeCommand.empty()) {
                row.insert_or_assign("resume_command", item.resumeCommand);
              }
              if (item.action == "suspend") {
                row.insert_or_assign("lock_before_suspend", item.lockBeforeSuspend);
              }
              behaviorTable.insert_or_assign(item.name, std::move(row));
              behaviorOrder.push_back(item.name);
            }
            table.insert_or_assign(key, std::move(behaviorTable));
            // Preserve user-defined behavior list order (table iteration order is not
            // a reliable ordering source after round-trips).
            if (key == "behavior") {
              table.insert_or_assign("behavior_order", std::move(behaviorOrder));
            }
          } else if constexpr (std::is_same_v<T, std::vector<KeyChord>>) {
            toml::array array;
            for (const auto& item : concrete) {
              std::string serialized = keyChordToString(item);
              if (serialized.empty()) {
                continue;
              }
              array.push_back(std::move(serialized));
            }
            table.insert_or_assign(key, std::move(array));
          } else {
            table.insert_or_assign(key, concrete);
          }
        },
        value
    );
  }

  std::vector<std::string> barOrderNames(const std::vector<BarConfig>& bars) {
    std::vector<std::string> order;
    order.reserve(bars.size());
    for (const auto& bar : bars) {
      order.push_back(bar.name);
    }
    return order;
  }

  bool setBarOverrideOrder(toml::table& root, const std::vector<std::string>& order) {
    auto* barRoot = ensureTable(root, "bar");
    if (barRoot == nullptr) {
      return false;
    }
    insertOverrideValue(*barRoot, "order", order);
    return true;
  }

  const toml::node* findOverrideNode(const toml::table& root, const std::vector<std::string>& path) {
    const toml::table* table = &root;
    for (std::size_t i = 0; i < path.size(); ++i) {
      if (i + 1 == path.size()) {
        return table->get(path[i]);
      }
      auto* next = table->get_as<toml::table>(path[i]);
      if (next == nullptr) {
        return nullptr;
      }
      table = next;
    }
    return nullptr;
  }

  void pruneEmptyOverrideTables(
      toml::table& root, const std::vector<std::string>& changedPath, std::size_t preserveDepth = 0
  ) {
    if (changedPath.size() < 2) {
      return;
    }

    for (std::size_t depth = changedPath.size() - 1; depth > 0; --depth) {
      if (preserveDepth > 0 && depth <= preserveDepth) {
        break;
      }

      toml::table* parent = &root;
      for (std::size_t i = 0; i + 1 < depth; ++i) {
        parent = parent->get_as<toml::table>(changedPath[i]);
        if (parent == nullptr) {
          return;
        }
      }

      auto* node = parent->get(changedPath[depth - 1]);
      auto* table = node != nullptr ? node->as_table() : nullptr;
      if (table == nullptr || !table->empty()) {
        break;
      }
      parent->erase(changedPath[depth - 1]);
    }
  }

  bool eraseOverridePath(toml::table& root, const std::vector<std::string>& path, std::size_t preserveDepth = 0) {
    if (path.empty()) {
      return false;
    }

    toml::table* table = &root;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
      auto* next = table->get_as<toml::table>(path[i]);
      if (next == nullptr) {
        return false;
      }
      table = next;
    }

    if (table->erase(path.back()) == 0) {
      return false;
    }
    pruneEmptyOverrideTables(root, path, preserveDepth);
    return true;
  }

  bool overridePresenceIsSemantic(const std::vector<std::string>& path) {
    if (path.size() == 3 && path[0] == "widget" && path[2] == "type") {
      return true;
    }
    if (path.size() != 5 || path[0] != "bar" || path[2] != "monitor") {
      return false;
    }
    const auto& key = path[4];
    return key == "start" || key == "center" || key == "end";
  }

  std::vector<std::filesystem::path> sortedConfigTomlFiles(std::string_view configDir) {
    std::vector<std::filesystem::path> files;
    if (configDir.empty()) {
      return files;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(configDir, ec) || ec) {
      return files;
    }
    for (const auto& entry : std::filesystem::directory_iterator(configDir, ec)) {
      if (entry.is_regular_file() && entry.path().extension() == ".toml") {
        files.push_back(entry.path());
      }
    }
    std::sort(files.begin(), files.end());
    return files;
  }
} // namespace

void ConfigService::setThemeMode(ThemeMode mode) {
  if (m_overridesPath.empty()) {
    return;
  }

  auto* themeTbl = ensureTable(m_overridesTable, "theme");
  themeTbl->insert_or_assign("mode", std::string(enumToKey(kThemeModes, mode)));

  if (!writeOverridesToFile()) {
    kLog.warn("failed to write {}", m_overridesPath);
    return;
  }

  m_ownOverridesWritePending = true;

  // Rebuild Config and fan out reload callbacks so ThemeService transitions.
  loadAll();
  fireReloadCallbacks();
}

bool ConfigService::setThemeWallpaperScheme(std::string_view schemeRaw) {
  if (m_overridesPath.empty()) {
    return false;
  }

  const std::string scheme = StringUtils::trim(std::string(schemeRaw));
  if (scheme.empty() || !noctalia::theme::schemeFromString(scheme)) {
    return false;
  }

  auto* themeTbl = ensureTable(m_overridesTable, "theme");
  themeTbl->insert_or_assign("wallpaper_scheme", scheme);

  if (!writeOverridesToFile()) {
    kLog.warn("failed to write {}", m_overridesPath);
    return false;
  }

  m_ownOverridesWritePending = true;
  loadAll();
  fireReloadCallbacks();
  return true;
}

void ConfigService::setDockEnabled(bool enabled) {
  if (m_overridesPath.empty()) {
    return;
  }

  auto* dockTbl = ensureTable(m_overridesTable, "dock");
  const auto existing = (*dockTbl)["enabled"].value<bool>();
  if (existing.has_value() && *existing == enabled && m_config.dock.enabled == enabled) {
    return;
  }

  dockTbl->insert_or_assign("enabled", enabled);

  if (!writeOverridesToFile()) {
    kLog.warn("failed to write {}", m_overridesPath);
    return;
  }

  m_ownOverridesWritePending = true;

  loadAll();
  fireReloadCallbacks();
}

bool ConfigService::setDesktopWidgetsState(const DesktopWidgetsConfig& desktopWidgets) {
  if (m_overridesPath.empty()) {
    return false;
  }

  auto* desktopWidgetsTbl = ensureTable(m_overridesTable, "desktop_widgets");
  if (desktopWidgetsTbl == nullptr) {
    return false;
  }

  desktopWidgetsTbl->insert_or_assign("schema_version", static_cast<std::int64_t>(desktopWidgets.schemaVersion));

  toml::table grid;
  grid.insert_or_assign("visible", desktopWidgets.grid.visible);
  grid.insert_or_assign("cell_size", static_cast<std::int64_t>(desktopWidgets.grid.cellSize));
  grid.insert_or_assign("major_interval", static_cast<std::int64_t>(desktopWidgets.grid.majorInterval));
  desktopWidgetsTbl->insert_or_assign("grid", std::move(grid));

  toml::table widgets;
  toml::array widgetOrder;
  for (const auto& widget : desktopWidgets.widgets) {
    if (widget.id.empty() || widget.type.empty()) {
      continue;
    }
    widgets.insert_or_assign(widget.id, desktopWidgetTable(widget));
    widgetOrder.push_back(widget.id);
  }
  desktopWidgetsTbl->insert_or_assign("widget", std::move(widgets));
  desktopWidgetsTbl->insert_or_assign("widget_order", std::move(widgetOrder));

  if (!writeOverridesToFile()) {
    kLog.warn("failed to write {}", m_overridesPath);
    return false;
  }

  m_ownOverridesWritePending = true;
  loadAll();
  fireReloadCallbacks();
  return true;
}

bool ConfigService::markSetupWizardCompleted() {
  if (m_setupMarkerPath.empty()) {
    return false;
  }
  if (std::filesystem::exists(m_setupMarkerPath)) {
    return true;
  }

  std::ofstream out(m_setupMarkerPath, std::ios::trunc);
  if (!out.is_open()) {
    kLog.warn("failed to write {}", m_setupMarkerPath);
    return false;
  }
  return true;
}

bool ConfigService::hasOverride(const std::vector<std::string>& path) const {
  if (path.empty()) {
    return false;
  }
  return findOverrideNode(m_overridesTable, path) != nullptr;
}

bool ConfigService::hasEffectiveOverride(const std::vector<std::string>& path) const {
  if (path.empty() || findOverrideNode(m_overridesTable, path) == nullptr) {
    return false;
  }

  const std::string key = overrideCacheKey(path);
  if (const auto it = m_effectiveOverrideCache.find(key); it != m_effectiveOverrideCache.end()) {
    return it->second;
  }

  const bool effective = overridePathEffectiveInTable(path, m_overridesTable, &m_config);
  m_effectiveOverrideCache[key] = effective;
  return effective;
}

std::size_t ConfigService::overridePreserveDepthForPath(const std::vector<std::string>& path) const {
  if (path.size() > 4 && path[0] == "bar" && path[2] == "monitor" && isOverrideOnlyMonitorOverride(path[1], path[3])) {
    return 4;
  }
  if (path.size() > 4 && path[0] == "wallpaper" && path[2] == "monitor" && path[3].size() > 0) {
    return 4;
  }
  if (path.size() > 2 && path[0] == "bar" && isOverrideOnlyBar(path[1])) {
    return 2;
  }
  return 0;
}

std::optional<Config> ConfigService::configForOverrides(const toml::table& overrides) const {
  Config parsed;
  seedBuiltinWidgets(parsed);

  const auto files = sortedConfigTomlFiles(m_configDir);
  toml::table merged;
  for (const auto& path : files) {
    try {
      auto tbl = toml::parse_file(path.string());
      deepMerge(merged, tbl);
    } catch (const toml::parse_error& e) {
      kLog.warn(
          "skipping parse error in effective override comparison {}: {}", path.filename().string(), e.description()
      );
    }
  }

  deepMerge(merged, overrides);
  if (files.empty() && overrides.empty()) {
    parsed.idle.behaviors = defaultIdleBehaviors();
    parsed.bars.push_back(BarConfig{});
    parsed.controlCenter.shortcuts = defaultControlCenterShortcuts();
    parsed.shell.session.actions = defaultSessionPanelActions();
    return parsed;
  }

  try {
    parseTableInto(merged, parsed, false);
  } catch (const std::exception& e) {
    kLog.warn("effective override comparison parse failed: {}", e.what());
    return std::nullopt;
  }
  return parsed;
}

bool ConfigService::overridePathEffectiveInTable(
    const std::vector<std::string>& path, const toml::table& overrides, const Config* parsedWith
) const {
  if (path.empty() || findOverrideNode(overrides, path) == nullptr) {
    return false;
  }

  std::optional<Config> ownedWithOverride;
  if (parsedWith == nullptr) {
    ownedWithOverride = configForOverrides(overrides);
    if (!ownedWithOverride.has_value()) {
      return true;
    }
    parsedWith = &*ownedWithOverride;
  }

  toml::table withoutTable = overrides;
  eraseOverridePath(withoutTable, path, overridePreserveDepthForPath(path));
  auto withoutOverride = configForOverrides(withoutTable);
  if (!withoutOverride.has_value()) {
    return true;
  }

  if (isWidgetSettingOverridePath(path)) {
    return settings::widgetSettingOverrideIsEffective(path[1], path[2], *parsedWith, *withoutOverride);
  }

  return !configEqual(*parsedWith, *withoutOverride);
}

bool ConfigService::isOverrideOnlyBar(std::string_view name) const {
  if (name.empty() || !hasOverride({"bar", std::string(name)})) {
    return false;
  }
  return !m_configFileBarNames.contains(std::string(name));
}

bool ConfigService::canMoveBarOverride(std::string_view name, int direction) const {
  if (direction == 0 || name.empty()) {
    return false;
  }

  const auto barIt = std::find_if(m_config.bars.begin(), m_config.bars.end(), [name](const BarConfig& bar) {
    return bar.name == name;
  });
  if (barIt == m_config.bars.end()) {
    return false;
  }

  if (direction < 0) {
    return barIt != m_config.bars.begin();
  }

  return std::next(barIt) != m_config.bars.end();
}

bool ConfigService::canDeleteBarOverride(std::string_view name) const {
  return m_config.bars.size() > 1 && isOverrideOnlyBar(name);
}

bool ConfigService::isOverrideOnlyMonitorOverride(std::string_view barName, std::string_view match) const {
  if (barName.empty() || match.empty() || !hasOverride({"bar", std::string(barName), "monitor", std::string(match)})) {
    return false;
  }

  const auto barIt = m_configFileMonitorOverrideNames.find(std::string(barName));
  if (barIt == m_configFileMonitorOverrideNames.end()) {
    return true;
  }
  return !barIt->second.contains(std::string(match));
}

bool ConfigService::createBarOverride(std::string_view name) {
  if (m_overridesPath.empty() || name.empty()) {
    return false;
  }

  for (const auto& bar : m_config.bars) {
    if (bar.name == name) {
      return false;
    }
  }

  auto* barRoot = ensureTable(m_overridesTable, "bar");
  if (barRoot == nullptr || barRoot->get(std::string(name)) != nullptr) {
    return false;
  }

  if (m_configFileBarNames.empty() && barRoot->empty() && m_config.bars.size() == 1 &&
      m_config.bars.front().name == "default") {
    auto* defaultBar = ensureTable(*barRoot, "default");
    if (defaultBar == nullptr) {
      return false;
    }
    defaultBar->insert_or_assign("enabled", m_config.bars.front().enabled);
  }

  auto* barTbl = ensureTable(*barRoot, name);
  if (barTbl == nullptr) {
    return false;
  }
  barTbl->insert_or_assign("enabled", true);

  auto order = barOrderNames(m_config.bars);
  order.push_back(std::string(name));
  if (!setBarOverrideOrder(m_overridesTable, order)) {
    return false;
  }

  if (!writeOverridesToFile()) {
    kLog.warn("failed to write {}", m_overridesPath);
    return false;
  }

  m_ownOverridesWritePending = true;
  loadAll();
  fireReloadCallbacks();
  return true;
}

bool ConfigService::moveBarOverride(std::string_view name, int direction) {
  if (!canMoveBarOverride(name, direction)) {
    return false;
  }

  auto order = barOrderNames(m_config.bars);
  const auto currentIt = std::find(order.begin(), order.end(), std::string(name));
  if (currentIt == order.end()) {
    return false;
  }

  if (direction < 0) {
    std::iter_swap(currentIt, std::prev(currentIt));
  } else {
    std::iter_swap(currentIt, std::next(currentIt));
  }

  if (!setBarOverrideOrder(m_overridesTable, order)) {
    return false;
  }

  if (!writeOverridesToFile()) {
    kLog.warn("failed to write {}", m_overridesPath);
    return false;
  }

  m_ownOverridesWritePending = true;
  loadAll();
  fireReloadCallbacks();
  return true;
}

bool ConfigService::renameBarOverride(std::string_view oldName, std::string_view newName) {
  if (oldName.empty() || newName.empty() || oldName == newName || !isOverrideOnlyBar(oldName)) {
    return false;
  }

  for (const auto& bar : m_config.bars) {
    if (bar.name == newName) {
      return false;
    }
  }

  auto order = barOrderNames(m_config.bars);
  for (auto& item : order) {
    if (item == oldName) {
      item = std::string(newName);
      break;
    }
  }
  if (!setBarOverrideOrder(m_overridesTable, order)) {
    return false;
  }

  return renameOverrideTable({"bar", std::string(oldName)}, {"bar", std::string(newName)});
}

bool ConfigService::deleteBarOverride(std::string_view name) {
  if (!canDeleteBarOverride(name)) {
    return false;
  }
  auto order = barOrderNames(m_config.bars);
  std::erase(order, std::string(name));
  if (!setBarOverrideOrder(m_overridesTable, order)) {
    return false;
  }
  return clearOverride({"bar", std::string(name)});
}

bool ConfigService::createMonitorOverride(std::string_view barName, std::string_view match) {
  if (m_overridesPath.empty() || barName.empty() || match.empty()) {
    return false;
  }

  const auto barIt = std::find_if(m_config.bars.begin(), m_config.bars.end(), [barName](const BarConfig& bar) {
    return bar.name == barName;
  });
  if (barIt == m_config.bars.end()) {
    return false;
  }
  const auto monitorIt = std::find_if(
      barIt->monitorOverrides.begin(), barIt->monitorOverrides.end(),
      [match](const BarMonitorOverride& ovr) { return ovr.match == match; }
  );
  if (monitorIt != barIt->monitorOverrides.end()) {
    return false;
  }

  auto* barRoot = ensureTable(m_overridesTable, "bar");
  if (barRoot == nullptr) {
    return false;
  }
  auto* barTbl = ensureTable(*barRoot, barName);
  if (barTbl == nullptr) {
    return false;
  }
  auto* monitorRoot = ensureTable(*barTbl, "monitor");
  if (monitorRoot == nullptr || monitorRoot->get(std::string(match)) != nullptr) {
    return false;
  }
  if (ensureTable(*monitorRoot, match) == nullptr) {
    return false;
  }

  if (!writeOverridesToFile()) {
    kLog.warn("failed to write {}", m_overridesPath);
    return false;
  }

  m_ownOverridesWritePending = true;
  loadAll();
  fireReloadCallbacks();
  return true;
}

bool ConfigService::renameMonitorOverride(
    std::string_view barName, std::string_view oldMatch, std::string_view newMatch
) {
  if (barName.empty() || oldMatch.empty() || newMatch.empty() || oldMatch == newMatch ||
      !isOverrideOnlyMonitorOverride(barName, oldMatch)) {
    return false;
  }

  const auto barIt = std::find_if(m_config.bars.begin(), m_config.bars.end(), [barName](const BarConfig& bar) {
    return bar.name == barName;
  });
  if (barIt == m_config.bars.end()) {
    return false;
  }
  const auto monitorIt = std::find_if(
      barIt->monitorOverrides.begin(), barIt->monitorOverrides.end(),
      [newMatch](const BarMonitorOverride& ovr) { return ovr.match == newMatch; }
  );
  if (monitorIt != barIt->monitorOverrides.end()) {
    return false;
  }

  return renameOverrideTable(
      {"bar", std::string(barName), "monitor", std::string(oldMatch)},
      {"bar", std::string(barName), "monitor", std::string(newMatch)}
  );
}

bool ConfigService::deleteMonitorOverride(std::string_view barName, std::string_view match) {
  if (!isOverrideOnlyMonitorOverride(barName, match)) {
    return false;
  }
  return clearOverride({"bar", std::string(barName), "monitor", std::string(match)});
}

bool ConfigService::setOverride(const std::vector<std::string>& path, ConfigOverrideValue value) {
  std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
  overrides.emplace_back(path, std::move(value));
  return setOverrides(std::move(overrides));
}

bool ConfigService::setOverrides(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides) {
  if (m_overridesPath.empty() || overrides.empty()) {
    return false;
  }

  toml::table next = m_overridesTable;
  for (const auto& [path, value] : overrides) {
    if (path.empty()) {
      return false;
    }

    toml::table* table = &next;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
      table = ensureTable(*table, path[i]);
      if (table == nullptr) {
        return false;
      }
    }

    insertOverrideValue(*table, path.back(), value);
  }

  for (const auto& [path, value] : overrides) {
    if (!overridePresenceIsSemantic(path)) {
      bool shouldErase = false;
      if (isWidgetSettingOverridePath(path)) {
        const std::string widgetType = widgetTypeForOverridePath(next, path[1]);
        shouldErase = settings::widgetOverrideValueMatchesRegistryDefault(widgetType, path[2], value) ||
                      !overridePathEffectiveInTable(path, next);
      } else {
        shouldErase = !overridePathEffectiveInTable(path, next);
      }
      if (shouldErase) {
        eraseOverridePath(next, path, overridePreserveDepthForPath(path));
        if (path.size() == 2 && path[0] == "idle" && path[1] == "behavior") {
          eraseOverridePath(next, {"idle", "behavior_order"}, overridePreserveDepthForPath(path));
        }
      }
    }
  }

  toml::table previous = std::move(m_overridesTable);
  m_overridesTable = std::move(next);
  if (!writeOverridesToFile()) {
    m_overridesTable = std::move(previous);
    kLog.warn("failed to write {}", m_overridesPath);
    return false;
  }

  m_ownOverridesWritePending = true;
  extractWallpaperFromOverrides();
  loadAll();
  fireReloadCallbacks();
  return true;
}

bool ConfigService::clearOverride(const std::vector<std::string>& path) {
  if (m_overridesPath.empty() || path.empty()) {
    return false;
  }

  if (!eraseOverridePath(m_overridesTable, path, overridePreserveDepthForPath(path))) {
    return false;
  }
  if (path.size() == 2 && path[0] == "idle" && path[1] == "behavior") {
    eraseOverridePath(m_overridesTable, {"idle", "behavior_order"}, overridePreserveDepthForPath(path));
  }

  if (!writeOverridesToFile()) {
    kLog.warn("failed to write {}", m_overridesPath);
    return false;
  }

  m_ownOverridesWritePending = true;
  extractWallpaperFromOverrides();
  loadAll();
  fireReloadCallbacks();
  return true;
}

bool ConfigService::renameOverrideTable(
    const std::vector<std::string>& oldPath, const std::vector<std::string>& newPath
) {
  if (m_overridesPath.empty() || oldPath.empty() || newPath.empty() || oldPath == newPath) {
    return false;
  }

  toml::table* oldParent = &m_overridesTable;
  for (std::size_t i = 0; i + 1 < oldPath.size(); ++i) {
    auto* next = oldParent->get_as<toml::table>(oldPath[i]);
    if (next == nullptr) {
      return false;
    }
    oldParent = next;
  }

  toml::node* oldNode = oldParent->get(oldPath.back());
  if (oldNode == nullptr || oldNode->as_table() == nullptr) {
    return false;
  }

  toml::table* newParent = &m_overridesTable;
  for (std::size_t i = 0; i + 1 < newPath.size(); ++i) {
    newParent = ensureTable(*newParent, newPath[i]);
    if (newParent == nullptr) {
      return false;
    }
  }

  if (newParent->get(newPath.back()) != nullptr) {
    return false;
  }

  if (oldParent == newParent) {
    std::vector<std::pair<std::string, const toml::node*>> entries;
    entries.reserve(oldParent->size());
    for (const auto& [key, node] : *oldParent) {
      std::string entryKey(key.str());
      if (entryKey == oldPath.back()) {
        entryKey = newPath.back();
      }
      entries.emplace_back(std::move(entryKey), &node);
    }

    toml::table renamed;
    for (const auto& [key, node] : entries) {
      renamed.insert_or_assign(key, *node);
    }
    *oldParent = std::move(renamed);
  } else {
    newParent->insert_or_assign(newPath.back(), *oldNode);
    oldParent->erase(oldPath.back());
    pruneEmptyOverrideTables(m_overridesTable, oldPath);
  }

  if (!writeOverridesToFile()) {
    kLog.warn("failed to write {}", m_overridesPath);
    return false;
  }

  m_ownOverridesWritePending = true;
  extractWallpaperFromOverrides();
  loadAll();
  fireReloadCallbacks();
  return true;
}

std::string ConfigService::getWallpaperPath(const std::string& connectorName) const {
  auto it = m_monitorWallpaperPaths.find(connectorName);
  if (it != m_monitorWallpaperPaths.end()) {
    return it->second;
  }
  return m_defaultWallpaperPath;
}

std::string ConfigService::getDefaultWallpaperPath() const { return m_defaultWallpaperPath; }

std::string ConfigService::getPaletteWallpaperPath() const {
  if (!m_lastWallpaperPath.empty()) {
    return m_lastWallpaperPath;
  }
  return m_defaultWallpaperPath;
}

void ConfigService::setWallpaperChangeCallback(ChangeCallback callback) {
  m_wallpaperChangeCallback = std::move(callback);
}

void ConfigService::setWallpaperPath(const std::optional<std::string>& connectorName, const std::string& path) {
  if (m_overridesPath.empty()) {
    return;
  }

  bool changed = false;
  if (connectorName.has_value()) {
    auto it = m_monitorWallpaperPaths.find(*connectorName);
    if (it == m_monitorWallpaperPaths.end() || it->second != path) {
      m_monitorWallpaperPaths[*connectorName] = path;
      changed = true;
    }
  } else {
    if (m_defaultWallpaperPath != path) {
      m_defaultWallpaperPath = path;
      changed = true;
    }
  }

  // Track the most recently applied wallpaper so palette generation still has a usable image
  // when wallpaper management is only used on a subset of displays (no default path set).
  const bool lastChanged = (m_lastWallpaperPath != path);
  if (lastChanged) {
    m_lastWallpaperPath = path;
    changed = true;
  }

  if (!changed) {
    return;
  }

  // Mirror the change into the overrides table so writeOverridesToFile() serializes it.
  auto* wallpaperTbl = ensureTable(m_overridesTable, "wallpaper");
  if (connectorName.has_value()) {
    auto* monitorsTbl = ensureTable(*wallpaperTbl, "monitors");
    auto* monTbl = ensureTable(*monitorsTbl, *connectorName);
    monTbl->insert_or_assign("path", path);
  } else {
    auto* defaultTbl = ensureTable(*wallpaperTbl, "default");
    defaultTbl->insert_or_assign("path", path);
  }
  if (lastChanged) {
    auto* lastTbl = ensureTable(*wallpaperTbl, "last");
    lastTbl->insert_or_assign("path", path);
  }

  if (!writeOverridesToFile()) {
    kLog.warn("failed to write {}", m_overridesPath);
    return;
  }

  m_ownOverridesWritePending = true;
  if (m_wallpaperBatchDepth > 0) {
    m_wallpaperBatchDirty = true;
    return;
  }
  if (m_wallpaperChangeCallback) {
    m_wallpaperChangeCallback();
  }
}

void ConfigService::extractWallpaperFromOverrides() { extractWallpaperFromTable(m_overridesTable); }

void ConfigService::extractWallpaperFromTable(const toml::table& table) {
  m_defaultWallpaperPath.clear();
  m_lastWallpaperPath.clear();
  m_monitorWallpaperPaths.clear();

  if (auto* wpDefault = table["wallpaper"]["default"].as_table()) {
    if (auto v = (*wpDefault)["path"].value<std::string>()) {
      m_defaultWallpaperPath = FileUtils::expandUserPath(*v).string();
    }
  }
  if (auto* wpLast = table["wallpaper"]["last"].as_table()) {
    if (auto v = (*wpLast)["path"].value<std::string>()) {
      m_lastWallpaperPath = FileUtils::expandUserPath(*v).string();
    }
  }
  if (auto* monitors = table["wallpaper"]["monitors"].as_table()) {
    for (const auto& [key, value] : *monitors) {
      if (auto* monTbl = value.as_table()) {
        if (auto v = (*monTbl)["path"].value<std::string>()) {
          m_monitorWallpaperPaths[std::string(key.str())] = FileUtils::expandUserPath(*v).string();
        }
      }
    }
  }
}

bool ConfigService::writeOverridesToFile() {
  if (m_overridesPath.empty()) {
    return false;
  }
  toml::table output = m_overridesTable;

  const std::string tmpPath = m_overridesPath + ".tmp";
  {
    std::ofstream out(tmpPath, std::ios::trunc);
    if (!out.is_open()) {
      return false;
    }
    out << toml::toml_formatter{
        output, toml::toml_formatter::default_flags & ~toml::format_flags::allow_literal_strings
    };
    if (!out.good()) {
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmpPath, m_overridesPath, ec);
  if (ec) {
    std::filesystem::remove(tmpPath, ec);
    return false;
  }
  return true;
}
