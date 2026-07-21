#include "config/atomic_file.h"
#include "config/config_merge.h"
#include "config/config_service.h"
#include "config/config_validate.h"
#include "config/widget_config.h"
#include "core/files/resource_paths.h"
#include "core/input/key_chord.h"
#include "core/log.h"
#include "scripting/plugin_id.h"
#include "shell/settings/widget_settings_registry.h"
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
    for (const auto& [key, value] : a) {
      const auto it = b.find(key);
      if (it == b.end() || !widgetSettingEqual(value, it->second)) {
        return false;
      }
    }
    return true;
  }

  // PluginsConfig equality that compares the open-ended pluginSettings map with int/double coercion
  // (widgetSettingsEqual) instead of the defaulted operator== — same reason as widgets.
  bool pluginsConfigEqual(const PluginsConfig& a, const PluginsConfig& b) {
    if (a.sources != b.sources
        || a.enabled != b.enabled
        || a.autoUpdate != b.autoUpdate
        || a.pluginSettings.size() != b.pluginSettings.size()) {
      return false;
    }
    for (const auto& [id, aMap] : a.pluginSettings) {
      const auto it = b.pluginSettings.find(id);
      if (it == b.pluginSettings.end() || !widgetSettingsEqual(aMap, it->second)) {
        return false;
      }
    }
    return true;
  }

  // Like DesktopWidgetState's defaulted operator==, but compares the settings map with int/double coercion
  // (widgetSettingsEqual) instead of exact variant equality.
  bool desktopWidgetEqual(const DesktopWidgetState& a, const DesktopWidgetState& b) {
    return a.id == b.id
        && a.type == b.type
        && a.outputName == b.outputName
        && a.cx == b.cx
        && a.cy == b.cy
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
  // automatically — no list to keep in sync here.
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
      resolved.widgetCapsulePadding = std::clamp(static_cast<float>(*ovr.widgetCapsulePadding), 0.0f, 48.0f);
    }
    if (ovr.widgetCapsuleRadius.has_value()) {
      resolved.widgetCapsuleRadius = std::clamp(*ovr.widgetCapsuleRadius, 0.0, 80.0);
    }
    if (ovr.widgetCapsuleOpacity) {
      resolved.widgetCapsuleOpacity = std::clamp(static_cast<float>(*ovr.widgetCapsuleOpacity), 0.0f, 1.0f);
    }
    if (ovr.hoverHighlight) {
      resolved.hoverHighlight = *ovr.hoverHighlight;
    }
    if (ovr.deadZone.command) {
      resolved.deadZone.command = *ovr.deadZone.command;
    }
    if (ovr.deadZone.rightCommand) {
      resolved.deadZone.rightCommand = *ovr.deadZone.rightCommand;
    }
    if (ovr.deadZone.middleCommand) {
      resolved.deadZone.middleCommand = *ovr.deadZone.middleCommand;
    }
    if (ovr.deadZone.scrollUpCommand) {
      resolved.deadZone.scrollUpCommand = *ovr.deadZone.scrollUpCommand;
    }
    if (ovr.deadZone.scrollDownCommand) {
      resolved.deadZone.scrollDownCommand = *ovr.deadZone.scrollDownCommand;
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

  bool isPluginSettingOverridePath(const std::vector<std::string>& path) {
    // {"plugin_settings", pluginId, key}; pluginId is "author/plugin".
    return path.size() == 3 && path[0] == "plugin_settings";
  }

  // Override-effectiveness equality. Every config section uses its compiler-generated operator== (exact
  // member-wise compare) so that adding a field cannot silently break override persistence — the only
  // exceptions are the sections whose comparison carries semantics operator== can't express:
  //   - bars: monitor overrides are resolved + clamped before comparing (barConfigEqual)
  //   - widgets / desktop widgets: settings compared with int/double coercion (widgetMapEqual / desktopWidgetEqual)
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
        && a.keybinds == b.keybinds
        && a.nightlight == b.nightlight
        && a.location == b.location
        && a.idle == b.idle
        && a.hooks == b.hooks
        && a.theme == b.theme
        && a.accessibility == b.accessibility
        && a.controlCenter == b.controlCenter
        && pluginsConfigEqual(a.plugins, b.plugins);
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
          } else if constexpr (std::is_same_v<T, WidgetSettingStringMap>) {
            toml::table mapTable;
            std::vector<std::string> mapKeys;
            mapKeys.reserve(concrete.size());
            for (const auto& [mapKey, mapValue] : concrete) {
              (void)mapValue;
              mapKeys.push_back(mapKey);
            }
            std::ranges::sort(mapKeys);
            for (const auto& mapKey : mapKeys) {
              mapTable.insert_or_assign(mapKey, concrete.at(mapKey));
            }
            table.insert_or_assign(key, std::move(mapTable));
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
    widgetTable.insert_or_assign("box_width", static_cast<double>(widget.boxWidth));
    widgetTable.insert_or_assign("box_height", static_cast<double>(widget.boxHeight));
    widgetTable.insert_or_assign("rotation", static_cast<double>(widget.rotationRad));
    if (widget.flipX) {
      widgetTable.insert_or_assign("flip_x", true);
    }
    if (widget.flipY) {
      widgetTable.insert_or_assign("flip_y", true);
    }
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
              row.insert_or_assign("countdown_seconds", item.countdownSeconds);
              array.push_back(std::move(row));
            }
            table.insert_or_assign(key, std::move(array));
          } else if constexpr (std::is_same_v<T, std::vector<BarCapsuleGroupStyle>>) {
            toml::array array;
            for (const auto& item : concrete) {
              if (item.id.empty()) {
                continue;
              }
              toml::table row;
              row.insert_or_assign("id", item.id);
              row.insert_or_assign("enabled", item.enabled);
              toml::array members;
              for (const auto& member : item.members) {
                members.push_back(member);
              }
              row.insert_or_assign("members", std::move(members));
              row.insert_or_assign("fill", colorSpecToConfigString(item.fill));
              if (item.borderSpecified) {
                row.insert_or_assign(
                    "border", item.border.has_value() ? colorSpecToConfigString(*item.border) : std::string{}
                );
              }
              if (item.foreground.has_value()) {
                row.insert_or_assign("foreground", colorSpecToConfigString(*item.foreground));
              }
              row.insert_or_assign("padding", static_cast<double>(item.padding));
              if (item.radius.has_value()) {
                row.insert_or_assign("radius", static_cast<double>(*item.radius));
              }
              row.insert_or_assign("opacity", static_cast<double>(item.opacity));
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
              row.insert_or_assign("timeout", item.timeoutSeconds);
              if (!item.action.empty()) {
                row.insert_or_assign("action", item.action);
              }
              if (!item.command.empty()) {
                row.insert_or_assign("command", item.command);
              }
              if (!item.resumeCommand.empty()) {
                row.insert_or_assign("resume_command", item.resumeCommand);
              }
              if (item.action == "suspend" && !item.lockBeforeSuspend) {
                row.insert_or_assign("lock_before_suspend", false);
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
          } else if constexpr (std::is_same_v<T, std::vector<NotificationFilterConfig>>) {
            toml::table filterTable;
            toml::array filterOrder;
            for (const auto& item : concrete) {
              if (item.name.empty()) {
                continue;
              }
              toml::table row;
              row.insert_or_assign("enabled", item.enabled);
              if (!item.match.empty()) {
                row.insert_or_assign("match", item.match);
              }
              if (!item.matchContent.empty()) {
                row.insert_or_assign("match_content", item.matchContent);
              }
              row.insert_or_assign("show_toast", item.showToast);
              row.insert_or_assign("save_history", item.saveHistory);
              row.insert_or_assign("play_sound", item.playSound);
              row.insert_or_assign("allow_permanent", item.allowPermanent);
              if (item.overrideDuration.has_value()) {
                row.insert_or_assign("override_duration", static_cast<std::int64_t>(*item.overrideDuration));
              }
              if (!item.allowedUrgencies.empty()) {
                toml::array urgencies;
                for (const auto& urgency : item.allowedUrgencies) {
                  urgencies.push_back(urgency);
                }
                row.insert_or_assign("allowed_urgencies", std::move(urgencies));
              }
              filterTable.insert_or_assign(item.name, std::move(row));
              filterOrder.push_back(item.name);
            }
            table.insert_or_assign(key, std::move(filterTable));
            if (key == "filter") {
              table.insert_or_assign("filter_order", std::move(filterOrder));
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
      .keybinds = !(prev.keybinds == next.keybinds),
      .nightlight = !(prev.nightlight == next.nightlight),
      .location = !(prev.location == next.location),
      .idle = !(prev.idle == next.idle),
      .hooks = !(prev.hooks == next.hooks),
      .theme = !(prev.theme == next.theme),
      .controlCenter = !(prev.controlCenter == next.controlCenter),
      .plugins = !pluginsConfigEqual(prev.plugins, next.plugins),
      .hotCorners = !(prev.hotCorners == next.hotCorners),
      .accessibility = !(prev.accessibility == next.accessibility),
  };
}

void ConfigService::setPluginEnabled(std::string_view pluginId, bool enabled) {
  if (!scripting::isValidPluginId(pluginId)) {
    return;
  }
  if (m_overridesPath.empty()) {
    return;
  }

  const std::string id(pluginId);
  std::vector<std::string> next = m_config.plugins.enabled;
  const bool currentlyEnabled = std::ranges::contains(next, id);

  if (enabled) {
    if (currentlyEnabled) {
      return; // already enabled
    }
    next.push_back(id);
  } else {
    if (!currentlyEnabled) {
      return; // already disabled
    }
    std::erase(next, id);
  }

  toml::array enabledArray;
  for (const auto& plugin : next) {
    enabledArray.push_back(plugin);
  }
  auto* pluginsTbl = ensureTable(m_overridesTable, "plugins");
  pluginsTbl->insert_or_assign("enabled", std::move(enabledArray));

  if (!writeOverridesToFile()) {
    kLog.warn("failed to write {}", m_overridesPath);
    return;
  }

  m_ownOverridesWritePending = true;
  loadAll();
  fireReloadCallbacks();
}

void ConfigService::addPluginSource(const PluginSourceConfig& source) {
  if (m_overridesPath.empty() || !isValidPluginSourceName(source.name)) {
    return;
  }

  const auto sourceTable = [](const PluginSourceConfig& src) {
    toml::table entry;
    entry.insert_or_assign("name", src.name);
    entry.insert_or_assign("kind", std::string(enumToKey(kPluginSourceKinds, src.kind)));
    entry.insert_or_assign("location", src.location);
    if (!src.enabled) {
      entry.insert_or_assign("enabled", false);
    }
    return entry;
  };

  auto* pluginsTbl = ensureTable(m_overridesTable, "plugins");
  auto* arr = pluginsTbl->get_as<toml::array>("source");
  bool sourceWritten = false;
  if (arr == nullptr) {
    toml::array seededSources;
    for (const auto& existing : m_config.plugins.sources) {
      if (!isValidPluginSourceName(existing.name)) {
        continue;
      }
      seededSources.push_back(sourceTable(existing.name == source.name ? source : existing));
      sourceWritten = sourceWritten || existing.name == source.name;
    }
    auto [it, _] = pluginsTbl->insert_or_assign("source", std::move(seededSources));
    arr = it->second.as_array();
  }

  if (!sourceWritten) {
    // A source name is an identity, not a duplicate key. Replace any existing entry
    // in place — source order is precedence, so toggling enabled must not move the
    // source to the end. Append only when the name isn't present yet.
    bool replaced = false;
    for (auto it = arr->begin(); it != arr->end(); ++it) {
      const auto* tbl = it->as_table();
      const auto name = tbl != nullptr ? (*tbl)["name"].value<std::string>() : std::nullopt;
      if (name && *name == source.name) {
        arr->replace(it, sourceTable(source));
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      arr->push_back(sourceTable(source));
    }
  }

  if (!writeOverridesToFile()) {
    kLog.warn("failed to write {}", m_overridesPath);
    return;
  }
  m_ownOverridesWritePending = true;
  loadAll();
  fireReloadCallbacks();
}

void ConfigService::removePluginSource(std::string_view name) {
  if (m_overridesPath.empty()) {
    return;
  }
  auto* pluginsTbl = m_overridesTable.get_as<toml::table>("plugins");
  if (pluginsTbl == nullptr) {
    return;
  }
  auto* arr = pluginsTbl->get_as<toml::array>("source");
  if (arr == nullptr) {
    return;
  }

  bool removed = false;
  for (auto it = arr->begin(); it != arr->end();) {
    const auto* tbl = it->as_table();
    const auto entryName = tbl != nullptr ? (*tbl)["name"].value<std::string>() : std::nullopt;
    if (entryName && *entryName == name) {
      it = arr->erase(it);
      removed = true;
    } else {
      ++it;
    }
  }
  if (!removed) {
    return;
  }

  if (!writeOverridesToFile()) {
    kLog.warn("failed to write {}", m_overridesPath);
    return;
  }
  m_ownOverridesWritePending = true;
  loadAll();
  fireReloadCallbacks();
}

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

bool ConfigService::setThemeColorScheme(PaletteSource source, std::string_view valueRaw) {
  if (m_overridesPath.empty()) {
    return false;
  }

  const std::string value = StringUtils::trim(std::string(valueRaw));
  if (value.empty()) {
    return false;
  }

  switch (source) {
  case PaletteSource::Builtin:
    if (noctalia::theme::findBuiltinPalette(value) == nullptr) {
      return false;
    }
    break;
  case PaletteSource::Wallpaper:
    if (!noctalia::theme::schemeFromString(value)) {
      return false;
    }
    break;
  case PaletteSource::Community:
    break;
  case PaletteSource::Custom:
    if (!std::filesystem::exists(noctalia::theme::customPalettePath(value))) {
      return false;
    }
    break;
  }

  auto* themeTbl = ensureTable(m_overridesTable, "theme");
  themeTbl->insert_or_assign("source", std::string(enumToKey(kPaletteSources, source)));

  switch (source) {
  case PaletteSource::Builtin:
    themeTbl->insert_or_assign("builtin", value);
    break;
  case PaletteSource::Wallpaper:
    themeTbl->insert_or_assign("wallpaper_scheme", value);
    break;
  case PaletteSource::Community:
    themeTbl->insert_or_assign("community_palette", value);
    break;
  case PaletteSource::Custom:
    themeTbl->insert_or_assign("custom_palette", value);
    break;
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

void ConfigService::setPluginsAutoUpdate(bool enabled) {
  if (m_overridesPath.empty()) {
    return;
  }

  auto* pluginsTbl = ensureTable(m_overridesTable, "plugins");
  const auto existing = (*pluginsTbl)["auto_update"].value<bool>();
  if (existing.has_value() && *existing == enabled && m_config.plugins.autoUpdate == enabled) {
    return;
  }

  pluginsTbl->insert_or_assign("auto_update", enabled);

  if (!writeOverridesToFile()) {
    kLog.warn("failed to write {}", m_overridesPath);
    return;
  }

  m_ownOverridesWritePending = true;

  loadAll();
  fireReloadCallbacks();
}

namespace {

  void writeWidgetsPlacementToTable(
      toml::table& sectionTbl, const DesktopWidgetsGridState& grid, const std::vector<DesktopWidgetState>& widgets
  ) {
    toml::table gridTable;
    gridTable.insert_or_assign("visible", grid.visible);
    gridTable.insert_or_assign("cell_size", static_cast<std::int64_t>(grid.cellSize));
    gridTable.insert_or_assign("major_interval", static_cast<std::int64_t>(grid.majorInterval));
    sectionTbl.insert_or_assign("grid", std::move(gridTable));

    toml::table widgetTable;
    toml::array widgetOrder;
    for (const auto& widget : widgets) {
      if (widget.id.empty() || widget.type.empty()) {
        continue;
      }
      widgetTable.insert_or_assign(widget.id, desktopWidgetTable(widget));
      widgetOrder.push_back(widget.id);
    }
    sectionTbl.insert_or_assign("widget", std::move(widgetTable));
    sectionTbl.insert_or_assign("widget_order", std::move(widgetOrder));
  }

} // namespace

bool ConfigService::setDesktopWidgetsState(const DesktopWidgetsConfig& desktopWidgets) {
  if (m_overridesPath.empty()) {
    return false;
  }

  toml::table next = m_overridesTable;
  auto* desktopWidgetsTbl = ensureTable(next, "desktop_widgets");
  if (desktopWidgetsTbl == nullptr) {
    return false;
  }

  desktopWidgetsTbl->insert_or_assign("schema_version", static_cast<std::int64_t>(desktopWidgets.schemaVersion));
  writeWidgetsPlacementToTable(*desktopWidgetsTbl, desktopWidgets.grid, desktopWidgets.widgets);

  if (!validateOverrideMutation(next)) {
    return false;
  }
  toml::table previous = std::move(m_overridesTable);
  m_overridesTable = std::move(next);

  if (!writeOverridesToFile()) {
    m_overridesTable = std::move(previous);
    kLog.warn("failed to write {}", m_overridesPath);
    return false;
  }

  m_ownOverridesWritePending = true;
  loadAll();
  fireReloadCallbacks();
  return true;
}

bool ConfigService::setLockscreenWidgetsState(const LockscreenWidgetsConfig& lockscreenWidgets) {
  if (m_overridesPath.empty()) {
    return false;
  }

  toml::table next = m_overridesTable;
  auto* sectionTbl = ensureTable(next, "lockscreen_widgets");
  if (sectionTbl == nullptr) {
    return false;
  }

  sectionTbl->insert_or_assign("enabled", lockscreenWidgets.enabled);
  sectionTbl->insert_or_assign("schema_version", static_cast<std::int64_t>(lockscreenWidgets.schemaVersion));
  writeWidgetsPlacementToTable(*sectionTbl, lockscreenWidgets.grid, lockscreenWidgets.widgets);

  if (!validateOverrideMutation(next)) {
    return false;
  }
  toml::table previous = std::move(m_overridesTable);
  m_overridesTable = std::move(next);

  if (!writeOverridesToFile()) {
    m_overridesTable = std::move(previous);
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

  // The bundled wallpaper is served via firstRunWallpaperPath() until setup
  // completes. Persist it so finishing the wizard does not clear the desktop.
  if (!hasConfiguredWallpaper()) {
    const auto bundled = paths::assetPath("noctalia-wallpaper.png");
    std::error_code ec;
    if (std::filesystem::exists(bundled, ec)) {
      setWallpaperPath(std::nullopt, bundled.string());
    }
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
  noctalia::config::seedBuiltinWidgets(parsed);

  auto mergeResult = noctalia::config::mergeConfigWithIncludes(m_configDir);
  toml::table merged = std::move(mergeResult.merged);
  if (!mergeResult.firstError.empty()) {
    kLog.warn("skipping config error in effective override comparison: {}", mergeResult.firstError);
  }

  toml::table effectiveOverrides = overrides;
  noctalia::config::schema::Diagnostics migrationDiag;
  if (!effectiveOverrides.empty()) {
    const auto storedVersion = noctalia::config::storedConfigVersion(effectiveOverrides, migrationDiag);
    if (storedVersion.has_value()) {
      (void)noctalia::config::applyPendingConfigMigrations(effectiveOverrides, *storedVersion, migrationDiag);
    }
  }
  if (migrationDiag.hasErrors()) {
    kLog.warn("effective override comparison rejected invalid config_version");
    return std::nullopt;
  }
  deepMerge(merged, effectiveOverrides);
  merged.erase(noctalia::config::kConfigVersionKey);
  noctalia::config::LegacyConfigIssues issues;
  noctalia::config::normalizeLegacyConfig(merged, issues);
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

noctalia::config::schema::Diagnostics ConfigService::diagnosticsForOverrides(const toml::table& overrides) const {
  auto mergeResult = noctalia::config::mergeConfigWithIncludes(m_configDir);
  toml::table merged = std::move(mergeResult.merged);
  noctalia::config::schema::Diagnostics diagnostics;
  if (!mergeResult.firstError.empty()) {
    diagnostics.fatal("syntax", mergeResult.firstError, "config.syntax");
  }

  toml::table effectiveOverrides = overrides;
  if (!effectiveOverrides.empty()) {
    const auto storedVersion = noctalia::config::storedConfigVersion(effectiveOverrides, diagnostics);
    if (storedVersion.has_value()) {
      (void)noctalia::config::applyPendingConfigMigrations(effectiveOverrides, *storedVersion, diagnostics);
    }
  }
  deepMerge(merged, effectiveOverrides);
  merged.erase(noctalia::config::kConfigVersionKey);
  noctalia::config::LegacyConfigIssues issues;
  noctalia::config::normalizeLegacyConfig(merged, issues);
  for (const auto& issue : issues) {
    diagnostics.warn(issue.path, issue.message, "config.legacy");
  }

  auto semantic = noctalia::config::validateMergedConfig(merged);
  diagnostics.entries.insert(
      diagnostics.entries.end(), std::make_move_iterator(semantic.entries.begin()),
      std::make_move_iterator(semantic.entries.end())
  );
  return diagnostics;
}

bool ConfigService::validateOverrideMutation(
    const toml::table& candidateOverrides, const toml::table* baselineOverrides,
    const noctalia::config::schema::Diagnostics* candidateDiagnostics
) {
  m_lastMutationError.clear();
  if (!m_overridesParseError.empty()) {
    m_lastMutationError = m_overridesParseError;
    return false;
  }

  try {
    const auto baseline = diagnosticsForOverrides(baselineOverrides != nullptr ? *baselineOverrides : m_overridesTable);
    const auto computedCandidate = candidateDiagnostics == nullptr ? diagnosticsForOverrides(candidateOverrides)
                                                                   : noctalia::config::schema::Diagnostics{};
    const auto& candidate = candidateDiagnostics != nullptr ? *candidateDiagnostics : computedCandidate;
    const auto fatal = std::ranges::find_if(candidate.entries, [](const auto& entry) {
      return entry.severity == noctalia::config::schema::Diagnostics::Severity::Error
          && entry.recoveryScope == noctalia::config::schema::Diagnostics::RecoveryScope::Document;
    });
    if (fatal != candidate.entries.end()) {
      m_lastMutationError = fatal->path + ": " + fatal->message;
      return false;
    }
    const auto introduced = candidate.introducedErrorsComparedTo(baseline);
    if (!introduced.entries.empty()) {
      const auto& entry = introduced.entries.front();
      m_lastMutationError = entry.path + ": " + entry.message;
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

  if (isWidgetSettingOverridePath(path)) {
    return settings::widgetSettingOverrideIsEffective(path[1], path[2], *parsedWith, *withoutOverride);
  }

  if (isPluginSettingOverridePath(path)) {
    return settings::pluginSettingOverrideIsEffective(path[1], path[2], *parsedWith, *withoutOverride);
  }

  return !configEqual(*parsedWith, *withoutOverride);
}

bool ConfigService::isOverrideOnlyBar(std::string_view name) const {
  if (name.empty() || !hasOverride({"bar", std::string(name)})) {
    return false;
  }
  return !m_configFileBarNames.contains(std::string(name));
}

bool ConfigService::isOverrideOnlyCalendarAccount(std::string_view id) const {
  if (id.empty() || !hasOverride({"calendar", "account", std::string(id)})) {
    return false;
  }
  return !m_configFileCalendarAccountNames.contains(std::string(id));
}

bool ConfigService::canMoveBarOverride(std::string_view name, int direction) const {
  if (direction == 0 || name.empty()) {
    return false;
  }

  const auto barIt = std::ranges::find(m_config.bars, name, &BarConfig::name);
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

  if (m_configFileBarNames.empty()
      && barRoot->empty()
      && m_config.bars.size() == 1
      && m_config.bars.front().name == "default") {
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
  order.emplace_back(name);
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
  const auto currentIt = std::ranges::find(order, name);
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

  const auto barIt = std::ranges::find(m_config.bars, barName, &BarConfig::name);
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
  if (barName.empty()
      || oldMatch.empty()
      || newMatch.empty()
      || oldMatch == newMatch
      || !isOverrideOnlyMonitorOverride(barName, oldMatch)) {
    return false;
  }

  const auto barIt = std::ranges::find(m_config.bars, barName, &BarConfig::name);
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

bool ConfigService::deleteCalendarAccountOverride(std::string_view id) {
  if (!isOverrideOnlyCalendarAccount(id)) {
    return false;
  }
  return clearOverride({"calendar", "account", std::string(id)});
}

bool ConfigService::setOverride(const std::vector<std::string>& path, ConfigOverrideValue value) {
  return setOverride(path, std::move(value), nullptr);
}

bool ConfigService::validateOverride(
    const std::vector<std::string>& path, const ConfigOverrideValue& value, std::string* error
) {
  if (path.empty()) {
    if (error != nullptr) {
      *error = "invalid empty setting path";
    }
    return false;
  }

  toml::table candidate = m_overridesTable;
  toml::table* table = &candidate;
  for (std::size_t i = 0; i + 1 < path.size(); ++i) {
    table = ensureTable(*table, path[i]);
    if (table == nullptr) {
      if (error != nullptr) {
        *error = "setting path conflicts with a non-table value";
      }
      return false;
    }
  }
  insertOverrideValue(*table, path.back(), value);
  const auto candidateDiagnostics = diagnosticsForOverrides(candidate);
  const std::string settingPath = overrideCacheKey(path);
  const auto fieldError = std::ranges::find_if(candidateDiagnostics.entries, [&](const auto& entry) {
    return entry.severity == noctalia::config::schema::Diagnostics::Severity::Error && entry.path == settingPath;
  });
  if (fieldError != candidateDiagnostics.entries.end()) {
    m_lastMutationError = fieldError->path + ": " + fieldError->message;
    if (error != nullptr) {
      *error = m_lastMutationError;
    }
    return false;
  }

  const bool valid = validateOverrideMutation(candidate, nullptr, &candidateDiagnostics);
  if (!valid && error != nullptr) {
    *error = m_lastMutationError;
  }
  return valid;
}

bool ConfigService::setOverride(const std::vector<std::string>& path, ConfigOverrideValue value, bool* changed) {
  std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
  overrides.emplace_back(path, std::move(value));
  return setOverrides(std::move(overrides), changed);
}

bool ConfigService::setOverrides(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides) {
  return setOverrides(std::move(overrides), nullptr);
}

bool ConfigService::setOverrides(
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides, bool* changed
) {
  if (changed != nullptr) {
    *changed = false;
  }
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
      shouldErase = !overridePathEffectiveInTable(path, next);
      if (shouldErase) {
        eraseOverridePath(next, path, overridePreserveDepthForPath(path));
        if (path.size() == 2 && path[0] == "idle" && path[1] == "behavior") {
          eraseOverridePath(next, {"idle", "behavior_order"}, overridePreserveDepthForPath(path));
        }
      }
    }
  }

  if (next == m_overridesTable) {
    m_lastMutationError.clear();
    return true;
  }

  if (!validateOverrideMutation(next)) {
    return false;
  }

  toml::table previous = std::move(m_overridesTable);
  m_overridesTable = std::move(next);
  if (!writeOverridesToFile()) {
    m_overridesTable = std::move(previous);
    kLog.warn("failed to write {}", m_overridesPath);
    return false;
  }

  m_ownOverridesWritePending = true;
  if (changed != nullptr) {
    *changed = true;
  }
  extractWallpaperFromOverrides();
  loadAll();
  fireReloadCallbacks();
  return true;
}

bool ConfigService::clearOverride(const std::vector<std::string>& path) {
  bool changed = false;
  if (!clearOverrides({path}, &changed)) {
    return false;
  }
  return changed;
}

bool ConfigService::clearOverrides(const std::vector<std::vector<std::string>>& paths, bool* changed) {
  if (changed != nullptr) {
    *changed = false;
  }
  if (m_overridesPath.empty()) {
    return false;
  }

  toml::table next = m_overridesTable;
  bool anyChanged = false;
  for (const auto& path : paths) {
    if (path.empty()) {
      return false;
    }
    if (eraseOverridePath(next, path, overridePreserveDepthForPath(path))) {
      anyChanged = true;
      if (path.size() == 2 && path[0] == "idle" && path[1] == "behavior") {
        eraseOverridePath(next, {"idle", "behavior_order"}, overridePreserveDepthForPath(path));
      }
    }
  }

  if (!anyChanged) {
    m_lastMutationError.clear();
    return true;
  }

  if (!validateOverrideMutation(next)) {
    return false;
  }

  toml::table previous = std::move(m_overridesTable);
  m_overridesTable = std::move(next);
  if (!writeOverridesToFile()) {
    m_overridesTable = std::move(previous);
    kLog.warn("failed to write {}", m_overridesPath);
    return false;
  }

  m_ownOverridesWritePending = true;
  if (changed != nullptr) {
    *changed = true;
  }
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
  const auto path = paths::assetPath("noctalia-wallpaper.png");
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return {};
  }
  return path.string();
}

std::string ConfigService::getWallpaperPath(const std::string& connectorName) const {
  if (const std::string bundled = firstRunWallpaperPath(); !bundled.empty()) {
    return bundled;
  }
  auto it = m_monitorWallpaperPaths.find(connectorName);
  if (it != m_monitorWallpaperPaths.end()) {
    return it->second;
  }
  return m_defaultWallpaperPath;
}

std::string ConfigService::getDefaultWallpaperPath() const {
  if (const std::string bundled = firstRunWallpaperPath(); !bundled.empty()) {
    return bundled;
  }
  return m_defaultWallpaperPath;
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

std::string ConfigService::getGreeterSyncWallpaperPath() const {
  const std::string palettePath = getPaletteWallpaperPath();
  if (!palettePath.empty()) {
    return palettePath;
  }

  std::vector<std::string> connectors;
  connectors.reserve(m_monitorWallpaperPaths.size());
  for (const auto& [connector, path] : m_monitorWallpaperPaths) {
    if (!path.empty()) {
      connectors.push_back(connector);
    }
  }
  std::ranges::sort(connectors);
  for (const std::string& connector : connectors) {
    return m_monitorWallpaperPaths.at(connector);
  }
  return {};
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
  m_wallpaperFavorites.clear();

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
  if (auto* favorites = table["wallpaper"]["favorite"].as_array()) {
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
          favorite.themeMode = *parsed;
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

void ConfigService::syncWallpaperFavoritesToOverridesTable() {
  auto* wallpaperTbl = ensureTable(m_overridesTable, "wallpaper");
  if (m_wallpaperFavorites.empty()) {
    wallpaperTbl->erase("favorite");
    return;
  }

  toml::array favoritesArray;
  favoritesArray.reserve(m_wallpaperFavorites.size());
  for (const auto& favorite : m_wallpaperFavorites) {
    toml::table entry;
    entry.insert("path", favorite.path);
    entry.insert("theme_mode", std::string(enumToKey(kThemeModes, favorite.themeMode)));
    if (favorite.paletteSource.has_value()) {
      entry.insert("palette_source", std::string(enumToKey(kPaletteSources, *favorite.paletteSource)));
      switch (*favorite.paletteSource) {
      case PaletteSource::Builtin:
        if (!favorite.builtinPalette.empty()) {
          entry.insert("builtin_palette", favorite.builtinPalette);
        }
        break;
      case PaletteSource::Wallpaper:
        if (!favorite.wallpaperScheme.empty()) {
          entry.insert("wallpaper_scheme", favorite.wallpaperScheme);
        }
        break;
      case PaletteSource::Community:
        if (!favorite.communityPalette.empty()) {
          entry.insert("community_palette", favorite.communityPalette);
        }
        break;
      case PaletteSource::Custom:
        if (!favorite.customPalette.empty()) {
          entry.insert("custom_palette", favorite.customPalette);
        }
        break;
      }
    }
    favoritesArray.push_back(std::move(entry));
  }
  wallpaperTbl->insert_or_assign("favorite", std::move(favoritesArray));
}

const std::vector<WallpaperFavorite>& ConfigService::wallpaperFavorites() const noexcept {
  return m_wallpaperFavorites;
}

bool ConfigService::isWallpaperFavorite(std::string_view path) const {
  const std::string normalized = FileUtils::normalizeWallpaperPath(path);
  for (const auto& favorite : m_wallpaperFavorites) {
    if (favorite.path == normalized) {
      return true;
    }
  }
  return false;
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

void ConfigService::addWallpaperFavorite(std::string path, std::optional<WallpaperFavorite> preset) {
  if (m_overridesPath.empty()) {
    return;
  }

  path = FileUtils::normalizeWallpaperPath(path);
  if (path.empty()) {
    return;
  }

  std::erase_if(m_wallpaperFavorites, [&](const WallpaperFavorite& favorite) { return favorite.path == path; });
  WallpaperFavorite favorite = preset.value_or(WallpaperFavorite{});
  favorite.path = std::move(path);
  m_wallpaperFavorites.push_back(std::move(favorite));

  syncWallpaperFavoritesToOverridesTable();
  if (!writeOverridesToFile()) {
    kLog.warn("failed to write {}", m_overridesPath);
    return;
  }
  m_ownOverridesWritePending = true;
}

void ConfigService::removeWallpaperFavorite(std::string_view path) {
  if (m_overridesPath.empty()) {
    return;
  }

  const std::string normalized = FileUtils::normalizeWallpaperPath(path);
  const auto before = m_wallpaperFavorites.size();
  std::erase_if(m_wallpaperFavorites, [&](const WallpaperFavorite& favorite) { return favorite.path == normalized; });
  if (m_wallpaperFavorites.size() == before) {
    return;
  }

  syncWallpaperFavoritesToOverridesTable();
  if (!writeOverridesToFile()) {
    kLog.warn("failed to write {}", m_overridesPath);
    return;
  }
  m_ownOverridesWritePending = true;
}

void ConfigService::setWallpaperFavoriteThemeMode(std::string_view path, ThemeMode themeMode) {
  if (m_overridesPath.empty()) {
    return;
  }

  const std::string normalized = FileUtils::normalizeWallpaperPath(path);
  bool changed = false;
  for (auto& favorite : m_wallpaperFavorites) {
    if (favorite.path != normalized) {
      continue;
    }
    if (favorite.themeMode != themeMode) {
      favorite.themeMode = themeMode;
      changed = true;
    }
    break;
  }
  if (!changed) {
    return;
  }

  syncWallpaperFavoritesToOverridesTable();
  if (!writeOverridesToFile()) {
    kLog.warn("failed to write {}", m_overridesPath);
    return;
  }
  m_ownOverridesWritePending = true;
}

void ConfigService::setWallpaperFavoritePaletteSource(std::string_view path, std::optional<PaletteSource> source) {
  if (m_overridesPath.empty()) {
    return;
  }

  const std::string normalized = FileUtils::normalizeWallpaperPath(path);
  bool changed = false;
  for (auto& favorite : m_wallpaperFavorites) {
    if (favorite.path != normalized) {
      continue;
    }
    if (favorite.paletteSource != source) {
      favorite.paletteSource = source;
      changed = true;
    }
    if (source.has_value()) {
      switch (*source) {
      case PaletteSource::Builtin:
        if (favorite.builtinPalette.empty()) {
          favorite.builtinPalette = m_config.theme.builtinPalette;
          changed = true;
        }
        break;
      case PaletteSource::Wallpaper:
        if (favorite.wallpaperScheme.empty()) {
          favorite.wallpaperScheme = m_config.theme.wallpaperScheme;
          changed = true;
        }
        break;
      case PaletteSource::Community:
        if (favorite.communityPalette.empty()) {
          favorite.communityPalette = m_config.theme.communityPalette;
          changed = true;
        }
        break;
      case PaletteSource::Custom:
        if (favorite.customPalette.empty()) {
          favorite.customPalette = m_config.theme.customPalette;
          changed = true;
        }
        break;
      }
    }
    break;
  }
  if (!changed) {
    return;
  }

  syncWallpaperFavoritesToOverridesTable();
  if (!writeOverridesToFile()) {
    kLog.warn("failed to write {}", m_overridesPath);
    return;
  }
  m_ownOverridesWritePending = true;
}

void ConfigService::setWallpaperFavoritePaletteSelection(std::string_view path, std::string_view value) {
  if (m_overridesPath.empty()) {
    return;
  }

  const std::string normalized = FileUtils::normalizeWallpaperPath(path);
  const std::string selection(value);
  bool changed = false;
  for (auto& favorite : m_wallpaperFavorites) {
    if (favorite.path != normalized || !favorite.paletteSource.has_value()) {
      continue;
    }
    switch (*favorite.paletteSource) {
    case PaletteSource::Builtin:
      if (favorite.builtinPalette != selection) {
        favorite.builtinPalette = selection;
        changed = true;
      }
      break;
    case PaletteSource::Wallpaper:
      if (favorite.wallpaperScheme != selection) {
        favorite.wallpaperScheme = selection;
        changed = true;
      }
      break;
    case PaletteSource::Community:
      if (favorite.communityPalette != selection) {
        favorite.communityPalette = selection;
        changed = true;
      }
      break;
    case PaletteSource::Custom:
      if (favorite.customPalette != selection) {
        favorite.customPalette = selection;
        changed = true;
      }
      break;
    }
    break;
  }
  if (!changed) {
    return;
  }

  syncWallpaperFavoritesToOverridesTable();
  if (!writeOverridesToFile()) {
    kLog.warn("failed to write {}", m_overridesPath);
    return;
  }
  m_ownOverridesWritePending = true;
}

void ConfigService::applyWallpaperSelection(
    const std::optional<std::string>& connectorName, const std::string& path, const WallpaperFavorite* applyTheme,
    const std::vector<std::string>& allConnectors
) {
  if (m_overridesPath.empty()) {
    return;
  }

  bool changed = false;

  if (applyTheme != nullptr) {
    auto* themeTbl = ensureTable(m_overridesTable, "theme");
    if (m_config.theme.mode != applyTheme->themeMode) {
      themeTbl->insert_or_assign("mode", std::string(enumToKey(kThemeModes, applyTheme->themeMode)));
      changed = true;
    }

    if (applyTheme->paletteSource.has_value()) {
      const PaletteSource source = *applyTheme->paletteSource;
      if (m_config.theme.source != source) {
        themeTbl->insert_or_assign("source", std::string(enumToKey(kPaletteSources, source)));
        changed = true;
      }
      switch (source) {
      case PaletteSource::Builtin:
        if (!applyTheme->builtinPalette.empty() && m_config.theme.builtinPalette != applyTheme->builtinPalette) {
          themeTbl->insert_or_assign("builtin", applyTheme->builtinPalette);
          changed = true;
        }
        break;
      case PaletteSource::Wallpaper:
        if (!applyTheme->wallpaperScheme.empty() && m_config.theme.wallpaperScheme != applyTheme->wallpaperScheme) {
          themeTbl->insert_or_assign("wallpaper_scheme", applyTheme->wallpaperScheme);
          changed = true;
        }
        break;
      case PaletteSource::Community:
        if (!applyTheme->communityPalette.empty() && m_config.theme.communityPalette != applyTheme->communityPalette) {
          themeTbl->insert_or_assign("community_palette", applyTheme->communityPalette);
          changed = true;
        }
        break;
      case PaletteSource::Custom:
        if (!applyTheme->customPalette.empty() && m_config.theme.customPalette != applyTheme->customPalette) {
          themeTbl->insert_or_assign("custom_palette", applyTheme->customPalette);
          changed = true;
        }
        break;
      }
    }
  }

  auto* wallpaperTbl = ensureTable(m_overridesTable, "wallpaper");

  if (connectorName.has_value() && !connectorName->empty()) {
    auto it = m_monitorWallpaperPaths.find(*connectorName);
    if (it == m_monitorWallpaperPaths.end() || it->second != path) {
      m_monitorWallpaperPaths[*connectorName] = path;
      changed = true;
    }
    auto* monitorsTbl = ensureTable(*wallpaperTbl, "monitors");
    auto* monTbl = ensureTable(*monitorsTbl, *connectorName);
    monTbl->insert_or_assign("path", path);
  } else {
    for (const auto& connector : allConnectors) {
      if (connector.empty()) {
        continue;
      }
      auto it = m_monitorWallpaperPaths.find(connector);
      if (it == m_monitorWallpaperPaths.end() || it->second != path) {
        m_monitorWallpaperPaths[connector] = path;
        changed = true;
      }
      auto* monitorsTbl = ensureTable(*wallpaperTbl, "monitors");
      auto* monTbl = ensureTable(*monitorsTbl, connector);
      monTbl->insert_or_assign("path", path);
    }
    if (m_defaultWallpaperPath != path) {
      m_defaultWallpaperPath = path;
      changed = true;
    }
    auto* defaultTbl = ensureTable(*wallpaperTbl, "default");
    defaultTbl->insert_or_assign("path", path);
  }

  if (m_lastWallpaperPath != path) {
    m_lastWallpaperPath = path;
    changed = true;
    auto* lastTbl = ensureTable(*wallpaperTbl, "last");
    lastTbl->insert_or_assign("path", path);
  }

  if (!changed) {
    return;
  }

  if (!writeOverridesToFile()) {
    kLog.warn("failed to write {}", m_overridesPath);
    return;
  }

  m_ownOverridesWritePending = true;
  loadAll();
  fireReloadCallbacks();
  if (m_wallpaperChangeCallback) {
    m_wallpaperChangeCallback();
  }
}

bool ConfigService::writeOverridesToFile() {
  if (m_overridesPath.empty()) {
    return false;
  }
  if (!validateOverrideMutation(m_overridesTable, &m_persistedOverridesTable)) {
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
