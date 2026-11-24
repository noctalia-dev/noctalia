#include "config/atomic_file.h"
#include "config/config_merge.h"
#include "config/config_service.h"
#include "config/schema/ranges.h"
#include "config/widget_config.h"
#include "core/files/resource_paths.h"
#include "core/input/key_chord.h"
#include "core/log.h"
#include "theme/builtin_palettes.h"
#include "theme/custom_palettes.h"
#include "theme/scheme.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
  constexpr Logger kLog("config");

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
      return aNum.has_value() && bNum.has_value() && *aNum == *bNum;
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
    return std::ranges::all_of(a, [&](const auto& entry) {
      const auto& [key, value] = entry;
      const auto it = b.find(key);
      return it != b.end() && widgetSettingEqual(value, it->second);
    });
  }

  // PluginsConfig equality that compares the open-ended pluginSettings map with int/double coercion
  // (widgetSettingsEqual) instead of the defaulted operator==, same reason as widgets.
  bool pluginsConfigEqual(const PluginsConfig& a, const PluginsConfig& b) {
    if (a.sources != b.sources
        || a.enabled != b.enabled
        || a.autoUpdate != b.autoUpdate
        || a.pluginSettings.size() != b.pluginSettings.size()) {
      return false;
    }
    return std::ranges::all_of(a.pluginSettings, [&](const auto& entry) {
      const auto& [id, aMap] = entry;
      const auto it = b.pluginSettings.find(id);
      return it != b.pluginSettings.end() && widgetSettingsEqual(aMap, it->second);
    });
  }

  // Like DesktopWidgetState's defaulted operator==, but compares the settings map with int/double coercion
  // (widgetSettingsEqual) instead of exact variant equality.
  bool desktopWidgetEqual(const DesktopWidgetState& a, const DesktopWidgetState& b) {
    return a.id == b.id
        && a.type == b.type
        && a.outputName == b.outputName
        && a.cx == b.cx
        && a.cy == b.cy
        && a.placementWidth == b.placementWidth
        && a.placementHeight == b.placementHeight
        && a.boxWidth == b.boxWidth
        && a.boxHeight == b.boxHeight
        && a.rotationRad == b.rotationRad
        && a.flipX == b.flipX
        && a.flipY == b.flipY
        && a.enabled == b.enabled
        && widgetSettingsEqual(a.settings, b.settings);
  }

  bool desktopWidgetsConfigEqual(const DesktopWidgetsConfig& a, const DesktopWidgetsConfig& b) {
    return a.enabled == b.enabled
        && a.schemaVersion == b.schemaVersion
        && a.grid == b.grid
        && vectorEqual(a.widgets, b.widgets, desktopWidgetEqual);
  }

  bool lockscreenWidgetsConfigEqual(const LockscreenWidgetsConfig& a, const LockscreenWidgetsConfig& b) {
    return a.enabled == b.enabled
        && a.schemaVersion == b.schemaVersion
        && a.grid == b.grid
        && vectorEqual(a.widgets, b.widgets, desktopWidgetEqual);
  }

  // Compares two bars ignoring their monitor-override lists (those are resolved + compared separately by
  // barConfigEqual). BarConfig's defaulted operator== covers every field, so new bar fields participate
  // automatically; no list to keep in sync here.
  bool barBaseConfigEqual(const BarConfig& a, const BarConfig& b) {
    BarConfig aa = a;
    BarConfig bb = b;
    aa.monitorOverrides.clear();
    bb.monitorOverrides.clear();
    return aa == bb;
  }

  BarConfig applyMonitorOverrideForComparison(const BarConfig& base, const BarMonitorOverride& ovr) {
    BarConfig resolved = base;
    resolved.monitorOverrides.clear();
    if (ovr.position) {
      resolved.position = *ovr.position;
    }
    if (ovr.enabled) {
      resolved.enabled = *ovr.enabled;
    }
    if (ovr.autoHide) {
      resolved.autoHide = *ovr.autoHide;
    }
    if (ovr.autoHideDelayMs) {
      resolved.autoHideDelayMs = *ovr.autoHideDelayMs;
    }
    if (ovr.smartAutoHide) {
      resolved.smartAutoHide = *ovr.smartAutoHide;
    }
    if (ovr.showOnWorkspaceSwitch) {
      resolved.showOnWorkspaceSwitch = *ovr.showOnWorkspaceSwitch;
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
    if (ovr.transparencyMode)
      resolved.transparencyMode = *ovr.transparencyMode;
    if (ovr.surfaceMode)
      resolved.surfaceMode = *ovr.surfaceMode;
    if (ovr.dynamicNotifications)
      resolved.dynamicNotifications = *ovr.dynamicNotifications;
    if (ovr.dynamicTimers)
      resolved.dynamicTimers = *ovr.dynamicTimers;
    if (ovr.dynamicAlarms)
      resolved.dynamicAlarms = *ovr.dynamicAlarms;
    if (ovr.dynamicCalls)
      resolved.dynamicCalls = *ovr.dynamicCalls;
    if (ovr.dynamicTimerSeconds)
      resolved.dynamicTimerSeconds = *ovr.dynamicTimerSeconds;
    if (ovr.dynamicAlarmTime)
      resolved.dynamicAlarmTime = *ovr.dynamicAlarmTime;
    if (ovr.dynamicSnoozeSeconds)
      resolved.dynamicSnoozeSeconds = *ovr.dynamicSnoozeSeconds;
    if (ovr.dynamicActivityIndicators)
      resolved.dynamicActivityIndicators = *ovr.dynamicActivityIndicators;
    if (ovr.dynamicPrivacy)
      resolved.dynamicPrivacy = *ovr.dynamicPrivacy;
    if (ovr.dynamicDevices)
      resolved.dynamicDevices = *ovr.dynamicDevices;
    if (ovr.dynamicVisualizer)
      resolved.dynamicVisualizer = *ovr.dynamicVisualizer;
    if (ovr.dynamicMedia)
      resolved.dynamicMedia = *ovr.dynamicMedia;
    if (ovr.dynamicVolume)
      resolved.dynamicVolume = *ovr.dynamicVolume;
    if (ovr.dynamicBrightness)
      resolved.dynamicBrightness = *ovr.dynamicBrightness;
    if (ovr.dynamicHoverExpand)
      resolved.dynamicHoverExpand = *ovr.dynamicHoverExpand;
    if (ovr.dynamicDwellMs)
      resolved.dynamicDwellMs = *ovr.dynamicDwellMs;
    if (ovr.dynamicWidth)
      resolved.dynamicWidth = *ovr.dynamicWidth;
    if (ovr.dynamicClockFormat)
      resolved.dynamicClockFormat = *ovr.dynamicClockFormat;
    if (ovr.dynamicCenter)
      resolved.dynamicCenter = *ovr.dynamicCenter;
    if (ovr.compositorBlur) {
      resolved.compositorBlur = *ovr.compositorBlur;
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
    if (ovr.concaveEdgeCorners) {
      resolved.concaveEdgeCorners = *ovr.concaveEdgeCorners;
    }
    if (ovr.marginEnds) {
      resolved.marginEnds = *ovr.marginEnds;
    }
    if (ovr.marginEdge) {
      resolved.marginEdge = *ovr.marginEdge;
    }
    if (ovr.marginOppositeEdge) {
      resolved.marginOppositeEdge = *ovr.marginOppositeEdge;
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
    if (ovr.panelOverlap) {
      resolved.panelOverlap = *ovr.panelOverlap;
    }
    if (ovr.capsuleThickness) {
      resolved.capsuleThickness = *ovr.capsuleThickness;
    }
    if (ovr.fontFamily) {
      resolved.fontFamily = *ovr.fontFamily;
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
    if (ovr.fontScale) {
      resolved.fontScale = *ovr.fontScale;
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
    if (ovr.widgetIconColor) {
      resolved.widgetIconColor = *ovr.widgetIconColor;
    }
    if (ovr.widgetCapsuleGroups) {
      resolved.widgetCapsuleGroups = *ovr.widgetCapsuleGroups;
    }
    if (ovr.widgetCapsulePadding) {
      resolved.widgetCapsulePadding = std::clamp(static_cast<float>(*ovr.widgetCapsulePadding), 0.0F, 48.0F);
    }
    if (ovr.widgetCapsuleRadius.has_value()) {
      resolved.widgetCapsuleRadius = std::clamp(*ovr.widgetCapsuleRadius, 0.0, 80.0);
    }
    if (ovr.widgetCapsuleOpacity) {
      resolved.widgetCapsuleOpacity = std::clamp(static_cast<float>(*ovr.widgetCapsuleOpacity), 0.0F, 1.0F);
    }
    if (ovr.widgetCapsuleBorderWidth) {
      resolved.widgetCapsuleBorderWidth = nythera::config::schema::applyRange(
          *ovr.widgetCapsuleBorderWidth, nythera::config::schema::kBarCapsuleBorderWidthRange
      );
    }
    if (ovr.hoverHighlight) {
      resolved.hoverHighlight = *ovr.hoverHighlight;
    }
    if (ovr.showTooltip) {
      resolved.showTooltip = *ovr.showTooltip;
    }
    if (ovr.deadZone.actions) {
      resolved.deadZone.actions = *ovr.deadZone.actions;
    }
    if (ovr.deadZone.clickThrough) {
      resolved.deadZone.clickThrough = *ovr.deadZone.clickThrough;
    }
    return resolved;
  }

  bool barMonitorOverrideEqual(const BarConfig& base, const BarMonitorOverride& a, const BarMonitorOverride& b) {
    return a.match == b.match
        && barBaseConfigEqual(applyMonitorOverrideForComparison(base, a), applyMonitorOverrideForComparison(base, b));
  }

  bool barConfigEqual(const BarConfig& a, const BarConfig& b) {
    return barBaseConfigEqual(a, b)
        && vectorEqual(
               a.monitorOverrides, b.monitorOverrides,
               [&a](const BarMonitorOverride& lhs, const BarMonitorOverride& rhs) {
                 return barMonitorOverrideEqual(a, lhs, rhs);
               }
        );
  }

  bool widgetConfigEqual(const WidgetConfig& a, const WidgetConfig& b) {
    return a.type == b.type && widgetSettingsEqual(a.settings, b.settings) && a.tables == b.tables;
  }

  bool widgetMapEqual(
      const std::unordered_map<std::string, WidgetConfig>& a, const std::unordered_map<std::string, WidgetConfig>& b
  ) {
    if (a.size() != b.size()) {
      return false;
    }
    return std::ranges::all_of(a, [&](const auto& entry) {
      const auto& [key, value] = entry;
      const auto it = b.find(key);
      return it != b.end() && widgetConfigEqual(value, it->second);
    });
  }

  bool isWidgetSettingOverridePath(const std::vector<std::string>& path) {
    return path.size() == 3 && path[0] == "widget";
  }

  bool isPluginSettingOverridePath(const std::vector<std::string>& path) {
    // {"plugin_settings", pluginId, key}; pluginId is "author/plugin".
    return path.size() == 3 && path[0] == "plugin_settings";
  }

  bool keybindSetEqual(const std::vector<KeyChord>& a, const std::vector<KeyChord>& b, KeybindAction action) {
    if (a == b) {
      return true;
    }
    if (a.empty()) {
      return b == defaultKeybindSet(action);
    }
    if (b.empty()) {
      return a == defaultKeybindSet(action);
    }
    return false;
  }

  bool keybindsConfigEqual(const KeybindsConfig& a, const KeybindsConfig& b) {
    if (a == b) {
      return true;
    }
    return keybindSetEqual(a.validate, b.validate, KeybindAction::Validate)
        && keybindSetEqual(a.cancel, b.cancel, KeybindAction::Cancel)
        && keybindSetEqual(a.left, b.left, KeybindAction::Left)
        && keybindSetEqual(a.right, b.right, KeybindAction::Right)
        && keybindSetEqual(a.up, b.up, KeybindAction::Up)
        && keybindSetEqual(a.down, b.down, KeybindAction::Down)
        && keybindSetEqual(a.tabNext, b.tabNext, KeybindAction::TabNext)
        && keybindSetEqual(a.tabPrevious, b.tabPrevious, KeybindAction::TabPrevious)
        && keybindSetEqual(a.deleteEntry, b.deleteEntry, KeybindAction::Delete)
        && keybindSetEqual(a.copy, b.copy, KeybindAction::Copy)
        && keybindSetEqual(a.save, b.save, KeybindAction::Save);
  }

  // Override-effectiveness equality. Every config section uses its compiler-generated operator== (exact
  // member-wise compare) so that adding a field cannot silently break override persistence. The only
  // exceptions are the sections whose comparison carries semantics operator== can't express:
  //   - bars: monitor overrides are resolved + clamped before comparing (barConfigEqual)
  //   - widgets / desktop widgets: settings compared with int/double coercion (widgetMapEqual / desktopWidgetEqual)
  //   - keybinds: an empty configured set and its built-in default set have the same runtime behavior
  bool configEqual(const Config& a, const Config& b) {
    return vectorEqual(a.bars, b.bars, barConfigEqual)
        && widgetMapEqual(a.widgets, b.widgets)
        && desktopWidgetsConfigEqual(a.desktopWidgets, b.desktopWidgets)
        && a.hotCorners == b.hotCorners
        && lockscreenWidgetsConfigEqual(a.lockscreenWidgets, b.lockscreenWidgets)
        && a.wallpaper == b.wallpaper
        && a.backdrop == b.backdrop
        && a.lockscreen == b.lockscreen
        && a.dock == b.dock
        && a.shell == b.shell
        && a.osd == b.osd
        && a.notification == b.notification
        && a.weather == b.weather
        && a.calendar == b.calendar
        && a.system == b.system
        && a.audio == b.audio
        && a.brightness == b.brightness
        && a.battery == b.battery
        && a.bluetooth == b.bluetooth
        && keybindsConfigEqual(a.keybinds, b.keybinds)
        && a.nightlight == b.nightlight
        && a.location == b.location
        && a.idle == b.idle
        && a.hooks == b.hooks
        && a.theme == b.theme
        && a.accessibility == b.accessibility
        && a.controlCenter == b.controlCenter
        && pluginsConfigEqual(a.plugins, b.plugins);
  }

  const toml::node* findOverrideNode(const toml::table& root, const std::vector<std::string>& path) {
    const toml::table* table = &root;
    for (std::size_t i = 0; i < path.size(); ++i) {
      if (i + 1 == path.size()) {
        return table->get(path[i]);
      }
      const auto* next = table->get_as<toml::table>(path[i]);
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

  const BarConfig* findBarConfig(const Config& cfg, std::string_view name) {
    const auto it = std::ranges::find(cfg.bars, name, &BarConfig::name);
    return it != cfg.bars.end() ? &*it : nullptr;
  }

  const BarMonitorOverride* findBarMonitorOverride(const BarConfig& bar, std::string_view match) {
    const auto it = std::ranges::find(bar.monitorOverrides, match, &BarMonitorOverride::match);
    return it != bar.monitorOverrides.end() ? &*it : nullptr;
  }

  // {"bar", name, lane} or {"bar", name, "monitor", match, lane}, lane being a widget list.
  bool isBarLanePath(const std::vector<std::string>& path) {
    const bool barScope = path.size() == 3 && path[0] == "bar";
    const bool monitorScope = path.size() == 5 && path[0] == "bar" && path[2] == "monitor";
    if (!barScope && !monitorScope) {
      return false;
    }
    const std::string& lane = path.back();
    return lane == "start" || lane == "center" || lane == "end";
  }

  std::vector<std::string> capsuleGroupPathForBarLanePath(const std::vector<std::string>& lanePath) {
    std::vector<std::string> path(lanePath.begin(), lanePath.end() - 1);
    path.emplace_back("capsule_group");
    return path;
  }

  const std::vector<std::string>* barLaneWidgets(const Config& cfg, const std::vector<std::string>& lanePath) {
    const BarConfig* bar = findBarConfig(cfg, lanePath[1]);
    if (bar == nullptr) {
      return nullptr;
    }
    const std::string& lane = lanePath.back();
    if (lanePath.size() == 5) {
      const BarMonitorOverride* ovr = findBarMonitorOverride(*bar, lanePath[3]);
      if (ovr == nullptr) {
        return nullptr;
      }
      const std::optional<std::vector<std::string>>& laneOverride =
          lane == "start" ? ovr->startWidgets : (lane == "center" ? ovr->centerWidgets : ovr->endWidgets);
      if (laneOverride.has_value()) {
        return &*laneOverride;
      }
    }
    return lane == "start" ? &bar->startWidgets : (lane == "center" ? &bar->centerWidgets : &bar->endWidgets);
  }

  const std::vector<BarCapsuleGroupStyle>*
  barLaneCapsuleGroups(const Config& cfg, const std::vector<std::string>& lanePath) {
    const BarConfig* bar = findBarConfig(cfg, lanePath[1]);
    if (bar == nullptr) {
      return nullptr;
    }
    if (lanePath.size() == 5) {
      const BarMonitorOverride* ovr = findBarMonitorOverride(*bar, lanePath[3]);
      if (ovr != nullptr && ovr->widgetCapsuleGroups.has_value()) {
        return &*ovr->widgetCapsuleGroups;
      }
    }
    return &bar->widgetCapsuleGroups;
  }

  // Lane equality as the settings GUI presents it: the widget list plus the styles of the capsule
  // groups that list references. Groups the lane does not reference belong to another lane.
  bool barLaneContentEqual(const Config& a, const Config& b, const std::vector<std::string>& lanePath) {
    const std::vector<std::string>* laneA = barLaneWidgets(a, lanePath);
    const std::vector<std::string>* laneB = barLaneWidgets(b, lanePath);
    if (laneA == nullptr || laneB == nullptr) {
      return laneA == laneB;
    }
    if (*laneA != *laneB) {
      return false;
    }
    const std::vector<BarCapsuleGroupStyle>* groupsA = barLaneCapsuleGroups(a, lanePath);
    const std::vector<BarCapsuleGroupStyle>* groupsB = barLaneCapsuleGroups(b, lanePath);
    for (const std::string& entry : *laneA) {
      if (!isCapsuleGroupToken(entry)) {
        continue;
      }
      const std::string id = capsuleGroupTokenId(entry);
      const auto findGroup = [&id](const std::vector<BarCapsuleGroupStyle>* groups) -> const BarCapsuleGroupStyle* {
        if (groups == nullptr) {
          return nullptr;
        }
        const auto it = std::ranges::find(*groups, id, &BarCapsuleGroupStyle::id);
        return it != groups->end() ? &*it : nullptr;
      };
      const BarCapsuleGroupStyle* groupA = findGroup(groupsA);
      const BarCapsuleGroupStyle* groupB = findGroup(groupsB);
      if (groupA == nullptr || groupB == nullptr) {
        if (groupA != groupB) {
          return false;
        }
        continue;
      }
      if (*groupA != *groupB) {
        return false;
      }
    }
    return true;
  }

} // namespace

ConfigChangeSet computeConfigChangeSet(const Config& prev, const Config& next) {
  return ConfigChangeSet{
      .bars = !vectorEqual(prev.bars, next.bars, barConfigEqual),
      .widgets = !widgetMapEqual(prev.widgets, next.widgets),
      .desktopWidgets = !desktopWidgetsConfigEqual(prev.desktopWidgets, next.desktopWidgets),
      .lockscreenWidgets = !lockscreenWidgetsConfigEqual(prev.lockscreenWidgets, next.lockscreenWidgets),
      .wallpaper = !(prev.wallpaper == next.wallpaper),
      .backdrop = !(prev.backdrop == next.backdrop),
      .lockscreen = !(prev.lockscreen == next.lockscreen),
      .dock = !(prev.dock == next.dock),
      .shell = !(prev.shell == next.shell),
      .osd = !(prev.osd == next.osd),
      .notification = !(prev.notification == next.notification),
      .weather = !(prev.weather == next.weather),
      .calendar = !(prev.calendar == next.calendar),
      .system = !(prev.system == next.system),
      .audio = !(prev.audio == next.audio),
      .brightness = !(prev.brightness == next.brightness),
      .battery = !(prev.battery == next.battery),
      .bluetooth = !(prev.bluetooth == next.bluetooth),
      .keybinds = !keybindsConfigEqual(prev.keybinds, next.keybinds),
      .nightlight = !(prev.nightlight == next.nightlight),
      .location = !(prev.location == next.location),
      .idle = !(prev.idle == next.idle),
      .hooks = !(prev.hooks == next.hooks),
      .theme = !(prev.theme == next.theme),
      .controlCenter = !(prev.controlCenter == next.controlCenter),
      .plugins = !pluginsConfigEqual(prev.plugins, next.plugins),
      .hotCorners = !(prev.hotCorners == next.hotCorners),
      .storage = !(prev.storage == next.storage),
      .accessibility = !(prev.accessibility == next.accessibility),
  };
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

bool ConfigService::isOverrideOnlyBar(std::string_view name) const {
  if (name.empty() || !hasOverride({"bar", std::string(name)})) {
    return false;
  }
  return !m_configFileBarNames.contains(std::string(name));
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

bool ConfigService::hasEffectiveBarLaneOverride(const std::vector<std::string>& lanePath) const {
  if (!isBarLanePath(lanePath)) {
    return false;
  }
  if (hasEffectiveOverride(lanePath)) {
    return true;
  }

  // The lane list itself matches the config file, but a group token in it can still carry the
  // change: moving a widget into a lane's capsule group only edits the scope's capsule_group array.
  const std::vector<std::string> groupPath = capsuleGroupPathForBarLanePath(lanePath);
  if (findOverrideNode(m_overridesTable, groupPath) == nullptr) {
    return false;
  }

  const std::string key = "lane:" + overrideCacheKey(lanePath);
  if (const auto it = m_effectiveOverrideCache.find(key); it != m_effectiveOverrideCache.end()) {
    return it->second;
  }

  toml::table without = m_overridesTable;
  eraseOverridePath(without, lanePath, overridePreserveDepthForPath(lanePath));
  eraseOverridePath(without, groupPath, overridePreserveDepthForPath(groupPath));
  const auto baseline = configForOverrides(without);
  const bool effective = !baseline.has_value() || !barLaneContentEqual(m_config, *baseline, lanePath);
  m_effectiveOverrideCache[key] = effective;
  return effective;
}

std::size_t ConfigService::overridePreserveDepthForPath(const std::vector<std::string>& path) const {
  if (path.size() > 4 && path[0] == "bar" && path[2] == "monitor" && isOverrideOnlyMonitorOverride(path[1], path[3])) {
    return 4;
  }
  if (path.size() > 4 && path[0] == "wallpaper" && path[2] == "monitor" && !path[3].empty()) {
    return 4;
  }
  if (path.size() > 2 && path[0] == "bar" && isOverrideOnlyBar(path[1])) {
    return 2;
  }
  return 0;
}

std::optional<Config> ConfigService::configForOverrides(const toml::table& overrides) const {
  Config parsed;
  nythera::config::seedBuiltinWidgets(parsed);

  auto mergeResult = nythera::config::mergeConfigWithIncludes(m_configDir);
  toml::table merged = std::move(mergeResult.merged);
  if (!mergeResult.firstError.empty()) {
    kLog.warn(
        "skipping config error in effective override comparison: {}",
        mergeResult.firstErrorOrigin.prefixed(mergeResult.firstError)
    );
  }

  toml::table effectiveOverrides = overrides;
  nythera::config::schema::Diagnostics migrationDiag;
  if (!effectiveOverrides.empty()) {
    const auto storedVersion = nythera::config::storedConfigVersion(effectiveOverrides, migrationDiag);
    if (storedVersion.has_value()) {
      (void)nythera::config::applyPendingConfigMigrations(effectiveOverrides, merged, *storedVersion, migrationDiag);
    }
  }
  if (migrationDiag.hasErrors()) {
    kLog.warn("effective override comparison rejected invalid config_version");
    return std::nullopt;
  }
  deepMerge(merged, effectiveOverrides);
  merged.erase(nythera::config::kConfigVersionKey);
  nythera::config::LegacyConfigIssues issues;
  nythera::config::normalizeLegacyConfig(merged, issues, &effectiveOverrides);
  if (mergeResult.loadedFiles.empty() && overrides.empty()) {
    parsed.idle.behaviors = defaultIdleBehaviors();
    parsed.bars.push_back(BarConfig{});
    parsed.controlCenter.shortcuts = defaultControlCenterShortcuts();
    parsed.shell.session.actions = defaultSessionPanelActions();
    return parsed;
  }

  try {
    parseConfigTable(merged, parsed, false, false);
  } catch (const std::exception& e) {
    kLog.warn("effective override comparison parse failed: {}", e.what());
    return std::nullopt;
  }
  return parsed;
}

nythera::config::schema::Diagnostics ConfigService::diagnosticsForOverrides(const toml::table& overrides) const {
  auto mergeResult = nythera::config::mergeConfigWithIncludes(m_configDir);
  toml::table merged = std::move(mergeResult.merged);
  nythera::config::ConfigOriginIndex origins = std::move(mergeResult.origins);
  nythera::config::schema::Diagnostics diagnostics;
  if (!mergeResult.firstError.empty()) {
    diagnostics.fatalAt(std::move(mergeResult.firstErrorOrigin), "syntax", mergeResult.firstError, "config.syntax");
  }

  toml::table effectiveOverrides = overrides;
  if (!m_overridesPath.empty()) {
    origins.record(std::filesystem::path(m_overridesPath), overrides);
  }
  if (!effectiveOverrides.empty()) {
    const auto storedVersion = nythera::config::storedConfigVersion(effectiveOverrides, diagnostics);
    if (storedVersion.has_value()) {
      (void)nythera::config::applyPendingConfigMigrations(effectiveOverrides, merged, *storedVersion, diagnostics);
    }
  }
  deepMerge(merged, effectiveOverrides);
  merged.erase(nythera::config::kConfigVersionKey);
  nythera::config::LegacyConfigIssues issues;
  nythera::config::normalizeLegacyConfig(merged, issues, &effectiveOverrides);
  for (const auto& issue : issues) {
    diagnostics.warn(issue.path, issue.message, "config.legacy");
  }
  origins.annotate(diagnostics);

  auto semantic = m_validator ? m_validator(merged, origins) : nythera::config::schema::Diagnostics{};
  diagnostics.entries.insert(
      diagnostics.entries.end(), std::make_move_iterator(semantic.entries.begin()),
      std::make_move_iterator(semantic.entries.end())
  );
  return diagnostics;
}

bool ConfigService::validateOverrideMutation(const toml::table& candidateOverrides, const OverrideDiagnostics& known) {
  using nythera::config::schema::Diagnostics;
  m_lastMutationError.clear();
  if (!m_overridesParseError.empty()) {
    m_lastMutationError = m_overridesParseError.flatten(m_configDir);
    return false;
  }

  try {
    std::optional<Diagnostics> computedCandidate;
    if (known.candidate == nullptr) {
      computedCandidate = diagnosticsForOverrides(candidateOverrides);
    }
    const Diagnostics& candidate = known.candidate != nullptr ? *known.candidate : *computedCandidate;
    const auto fatal = std::ranges::find_if(candidate.entries, [](const auto& entry) {
      return entry.severity == Diagnostics::Severity::Error
          && entry.recoveryScope == Diagnostics::RecoveryScope::Document;
    });
    if (fatal != candidate.entries.end()) {
      m_lastMutationError = fatal->describeShort(m_configDir);
      return false;
    }
    std::optional<Diagnostics> computedBaseline;
    if (known.baseline == nullptr) {
      computedBaseline = diagnosticsForOverrides(m_persistedOverridesTable);
    }
    const Diagnostics& baseline = known.baseline != nullptr ? *known.baseline : *computedBaseline;
    const auto introduced = candidate.introducedErrorsComparedTo(baseline);
    if (!introduced.entries.empty()) {
      const auto& entry = introduced.entries.front();
      m_lastMutationError = entry.describeShort(m_configDir);
      return false;
    }
  } catch (const std::exception& e) {
    m_lastMutationError = e.what();
    return false;
  }
  return true;
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

  if ((isWidgetSettingOverridePath(path) || isPluginSettingOverridePath(path)) && m_effectivenessChecker) {
    return m_effectivenessChecker(path, *parsedWith, *withoutOverride);
  }

  return !configEqual(*parsedWith, *withoutOverride);
}

bool ConfigService::hasConfiguredWallpaper() const {
  if (!m_defaultWallpaperPath.empty() || !m_lastWallpaperPath.empty()) {
    return true;
  }
  return !m_monitorWallpaperPaths.empty();
}

std::string ConfigService::firstRunWallpaperPath() const {
  if (m_setupMarkerPath.empty() || std::filesystem::exists(m_setupMarkerPath)) {
    return {};
  }
  if (hasConfiguredWallpaper()) {
    return {};
  }
  const auto path = paths::assetPath("nythera-wallpaper.jpeg");
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return {};
  }
  return path.string();
}

std::string ConfigService::getPaletteWallpaperPath() const {
  if (const std::string bundled = firstRunWallpaperPath(); !bundled.empty()) {
    return bundled;
  }
  if (!m_lastWallpaperPath.empty()) {
    return m_lastWallpaperPath;
  }
  return m_defaultWallpaperPath;
}

void ConfigService::extractWallpaperFromOverrides() { extractWallpaperFromTable(m_overridesTable); }

void ConfigService::extractWallpaperFromTable(const toml::table& table) {
  m_defaultWallpaperPath.clear();
  m_lastWallpaperPath.clear();
  m_monitorWallpaperPaths.clear();
  m_wallpaperFavorites.clear();

  if (const auto* wpDefault = table["wallpaper"]["default"].as_table()) {
    if (auto v = (*wpDefault)["path"].value<std::string>()) {
      m_defaultWallpaperPath = FileUtils::expandUserPath(*v).string();
    }
  }
  if (const auto* wpLast = table["wallpaper"]["last"].as_table()) {
    if (auto v = (*wpLast)["path"].value<std::string>()) {
      m_lastWallpaperPath = FileUtils::expandUserPath(*v).string();
    }
  }
  if (const auto* monitors = table["wallpaper"]["monitors"].as_table()) {
    for (const auto& [key, value] : *monitors) {
      if (const auto* monTbl = value.as_table()) {
        if (auto v = (*monTbl)["path"].value<std::string>()) {
          m_monitorWallpaperPaths[std::string(key.str())] = FileUtils::expandUserPath(*v).string();
        }
      }
    }
  }
  if (const auto* favorites = table["wallpaper"]["favorite"].as_array()) {
    for (const auto& node : *favorites) {
      const auto* favTbl = node.as_table();
      if (favTbl == nullptr) {
        continue;
      }
      WallpaperFavorite favorite;
      if (auto path = (*favTbl)["path"].value<std::string>()) {
        favorite.path = FileUtils::normalizeWallpaperPath(*path);
      }
      if (favorite.path.empty()) {
        continue;
      }
      if (auto modeKey = (*favTbl)["theme_mode"].value<std::string>()) {
        if (auto parsed = enumFromKey(kThemeModes, *modeKey)) {
          favorite.themeMode = parsed;
        }
      }
      if (auto sourceKey = (*favTbl)["palette_source"].value<std::string>()) {
        if (auto parsed = enumFromKey(kPaletteSources, *sourceKey)) {
          favorite.paletteSource = parsed;
        }
      }
      if (auto v = (*favTbl)["builtin_palette"].value<std::string>()) {
        favorite.builtinPalette = *v;
      }
      if (auto v = (*favTbl)["community_palette"].value<std::string>()) {
        favorite.communityPalette = *v;
      }
      if (auto v = (*favTbl)["custom_palette"].value<std::string>()) {
        favorite.customPalette = *v;
      }
      if (auto v = (*favTbl)["wallpaper_scheme"].value<std::string>()) {
        favorite.wallpaperScheme = *v;
      }
      m_wallpaperFavorites.push_back(std::move(favorite));
    }
  }
}

const std::vector<WallpaperFavorite>& ConfigService::wallpaperFavorites() const noexcept {
  return m_wallpaperFavorites;
}

bool ConfigService::isWallpaperFavorite(std::string_view path) const {
  const std::string normalized = FileUtils::normalizeWallpaperPath(path);
  return std::ranges::any_of(m_wallpaperFavorites, [&](const auto& favorite) { return favorite.path == normalized; });
}

const WallpaperFavorite* ConfigService::wallpaperFavorite(std::string_view path) const {
  const std::string normalized = FileUtils::normalizeWallpaperPath(path);
  for (const auto& favorite : m_wallpaperFavorites) {
    if (favorite.path == normalized) {
      return &favorite;
    }
  }
  return nullptr;
}

bool ConfigService::writeOverridesToFile(const OverrideDiagnostics& known) {
  if (m_overridesPath.empty()) {
    m_overridesTable = m_persistedOverridesTable;
    return false;
  }
  if (!validateOverrideMutation(m_overridesTable, known)) {
    m_overridesTable = m_persistedOverridesTable;
    return false;
  }
  toml::table output = m_overridesTable;

  std::ostringstream out;
  out << toml::toml_formatter{output, toml::toml_formatter::default_flags & ~toml::format_flags::allow_literal_strings};
  if (!out.good()) {
    m_overridesTable = m_persistedOverridesTable;
    return false;
  }
  if (!writeTextFileAtomic(m_overridesPath, out.str())) {
    m_overridesTable = m_persistedOverridesTable;
    return false;
  }
  m_persistedOverridesTable = m_overridesTable;
  return true;
}
