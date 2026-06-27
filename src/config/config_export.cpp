#include "config/config_export.h"

#include "config/schema/config_schema.h"
#include "config/schema/engine.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace config_export {
  namespace schema = noctalia::config::schema;

  namespace {

    toml::array stringArray(const std::vector<std::string>& values) {
      toml::array array;
      for (const auto& value : values) {
        array.push_back(value);
      }
      return array;
    }

    void insertWidgetSettingValue(toml::table& table, std::string_view key, const WidgetSettingValue& value) {
      std::visit(
          [&](const auto& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, std::vector<std::string>>) {
              table.insert_or_assign(key, stringArray(concrete));
            } else {
              table.insert_or_assign(key, concrete);
            }
          },
          value
      );
    }

    toml::table widgetConfigTable(const WidgetConfig& widget) {
      toml::table table;
      if (!widget.type.empty()) {
        table.insert_or_assign("type", widget.type);
      }

      std::vector<std::string> keys;
      keys.reserve(widget.settings.size());
      for (const auto& [key, value] : widget.settings) {
        (void)value;
        keys.push_back(key);
      }
      std::ranges::sort(keys);

      for (const auto& key : keys) {
        insertWidgetSettingValue(table, key, widget.settings.at(key));
      }
      std::vector<std::string> tableKeys;
      tableKeys.reserve(widget.tables.size());
      for (const auto& [key, map] : widget.tables) {
        (void)map;
        tableKeys.push_back(key);
      }
      std::ranges::sort(tableKeys);
      for (const auto& key : tableKeys) {
        const auto& map = widget.tables.at(key);
        toml::table subtable;
        std::vector<std::string> mapKeys;
        mapKeys.reserve(map.size());
        for (const auto& [mapKey, value] : map) {
          (void)value;
          mapKeys.push_back(mapKey);
        }
        std::ranges::sort(mapKeys);
        for (const auto& mapKey : mapKeys) {
          const auto& value = map.at(mapKey);
          subtable.insert_or_assign(mapKey, value);
        }
        table.insert_or_assign(key, std::move(subtable));
      }
      return table;
    }

    BarConfig applyMonitorOverride(const BarConfig& base, const BarMonitorOverride& ovr) {
      BarConfig resolved = base;
      if (ovr.position)
        resolved.position = *ovr.position;
      if (ovr.enabled)
        resolved.enabled = *ovr.enabled;
      if (ovr.autoHide)
        resolved.autoHide = *ovr.autoHide;
      if (ovr.showOnWorkspaceSwitch)
        resolved.showOnWorkspaceSwitch = *ovr.showOnWorkspaceSwitch;
      if (ovr.reserveSpace)
        resolved.reserveSpace = *ovr.reserveSpace;
      if (ovr.layer)
        resolved.layer = *ovr.layer;
      if (ovr.thickness)
        resolved.thickness = *ovr.thickness;
      if (ovr.backgroundOpacity)
        resolved.backgroundOpacity = *ovr.backgroundOpacity;
      if (ovr.border)
        resolved.border = *ovr.border;
      if (ovr.borderWidth)
        resolved.borderWidth = *ovr.borderWidth;
      if (ovr.radius) {
        resolved.radius = *ovr.radius;
        resolved.radiusTopLeft = *ovr.radius;
        resolved.radiusTopRight = *ovr.radius;
        resolved.radiusBottomLeft = *ovr.radius;
        resolved.radiusBottomRight = *ovr.radius;
      }
      if (ovr.radiusTopLeft)
        resolved.radiusTopLeft = *ovr.radiusTopLeft;
      if (ovr.radiusTopRight)
        resolved.radiusTopRight = *ovr.radiusTopRight;
      if (ovr.radiusBottomLeft)
        resolved.radiusBottomLeft = *ovr.radiusBottomLeft;
      if (ovr.radiusBottomRight)
        resolved.radiusBottomRight = *ovr.radiusBottomRight;
      if (ovr.marginEnds)
        resolved.marginEnds = *ovr.marginEnds;
      if (ovr.marginEdge)
        resolved.marginEdge = *ovr.marginEdge;
      if (ovr.marginOppositeEdge)
        resolved.marginOppositeEdge = *ovr.marginOppositeEdge;
      if (ovr.padding)
        resolved.padding = *ovr.padding;
      if (ovr.widgetSpacing)
        resolved.widgetSpacing = *ovr.widgetSpacing;
      if (ovr.shadow)
        resolved.shadow = *ovr.shadow;
      if (ovr.contactShadow)
        resolved.contactShadow = *ovr.contactShadow;
      if (ovr.panelOverlap)
        resolved.panelOverlap = *ovr.panelOverlap;
      if (ovr.capsuleThickness)
        resolved.capsuleThickness = *ovr.capsuleThickness;
      if (ovr.fontFamily)
        resolved.fontFamily = *ovr.fontFamily;
      if (ovr.startWidgets)
        resolved.startWidgets = *ovr.startWidgets;
      if (ovr.centerWidgets)
        resolved.centerWidgets = *ovr.centerWidgets;
      if (ovr.endWidgets)
        resolved.endWidgets = *ovr.endWidgets;
      if (ovr.scale)
        resolved.scale = *ovr.scale;
      if (ovr.widgetCapsuleDefault)
        resolved.widgetCapsuleDefault = *ovr.widgetCapsuleDefault;
      if (ovr.widgetCapsuleFill)
        resolved.widgetCapsuleFill = *ovr.widgetCapsuleFill;
      if (ovr.widgetCapsuleBorderSpecified) {
        resolved.widgetCapsuleBorderSpecified = true;
        resolved.widgetCapsuleBorder = ovr.widgetCapsuleBorder;
      }
      if (ovr.widgetCapsuleForeground)
        resolved.widgetCapsuleForeground = *ovr.widgetCapsuleForeground;
      if (ovr.widgetColor)
        resolved.widgetColor = *ovr.widgetColor;
      if (ovr.widgetIconColor)
        resolved.widgetIconColor = *ovr.widgetIconColor;
      if (ovr.widgetCapsuleGroups)
        resolved.widgetCapsuleGroups = *ovr.widgetCapsuleGroups;
      if (ovr.widgetCapsulePadding)
        resolved.widgetCapsulePadding = static_cast<float>(*ovr.widgetCapsulePadding);
      if (ovr.widgetCapsuleRadius.has_value()) {
        resolved.widgetCapsuleRadius = ovr.widgetCapsuleRadius;
      }
      if (ovr.widgetCapsuleOpacity)
        resolved.widgetCapsuleOpacity = static_cast<float>(*ovr.widgetCapsuleOpacity);
      if (ovr.deadZone.command)
        resolved.deadZone.command = *ovr.deadZone.command;
      if (ovr.deadZone.rightCommand)
        resolved.deadZone.rightCommand = *ovr.deadZone.rightCommand;
      if (ovr.deadZone.middleCommand)
        resolved.deadZone.middleCommand = *ovr.deadZone.middleCommand;
      if (ovr.deadZone.scrollUpCommand)
        resolved.deadZone.scrollUpCommand = *ovr.deadZone.scrollUpCommand;
      if (ovr.deadZone.scrollDownCommand)
        resolved.deadZone.scrollDownCommand = *ovr.deadZone.scrollDownCommand;
      return resolved;
    }

    // Base bar always emits position; the concrete fields come from the schema.
    // Each monitor override is resolved against the base and flattened through the
    // same schema, with match + (optional) position written explicitly — the
    // override's own optional fields are never serialized directly.
    toml::table barConfigTable(const BarConfig& bar) {
      toml::table table = schema::writeTable(bar, schema::barFieldsSchema());
      table.insert_or_assign("position", bar.position);

      if (!bar.monitorOverrides.empty()) {
        toml::table monitors;
        for (const auto& ovr : bar.monitorOverrides) {
          if (ovr.match.empty()) {
            continue;
          }
          toml::table monitor = schema::writeTable(applyMonitorOverride(bar, ovr), schema::barFieldsSchema());
          monitor.insert_or_assign("match", ovr.match);
          if (ovr.position) {
            monitor.insert_or_assign("position", *ovr.position);
          }
          monitors.insert_or_assign(ovr.match, std::move(monitor));
        }
        table.insert_or_assign("monitor", std::move(monitors));
      }
      return table;
    }

    toml::table widgetsPlacementTable(
        bool enabled, std::int32_t schemaVersion, const DesktopWidgetsGridState& grid,
        const std::vector<DesktopWidgetState>& widgets
    ) {
      toml::table table;
      table.insert_or_assign("enabled", enabled);
      table.insert_or_assign("schema_version", static_cast<std::int64_t>(schemaVersion));

      toml::table gridTable;
      gridTable.insert_or_assign("visible", grid.visible);
      gridTable.insert_or_assign("cell_size", static_cast<std::int64_t>(grid.cellSize));
      gridTable.insert_or_assign("major_interval", static_cast<std::int64_t>(grid.majorInterval));
      table.insert_or_assign("grid", std::move(gridTable));

      if (!widgets.empty()) {
        toml::array order;
        toml::table widgetTable;
        for (const auto& widget : widgets) {
          if (widget.id.empty()) {
            continue;
          }
          order.push_back(widget.id);
          toml::table item;
          item.insert_or_assign("type", widget.type);
          item.insert_or_assign("output", widget.outputName);
          item.insert_or_assign("cx", static_cast<double>(widget.cx));
          item.insert_or_assign("cy", static_cast<double>(widget.cy));
          item.insert_or_assign("box_width", static_cast<double>(widget.boxWidth));
          item.insert_or_assign("box_height", static_cast<double>(widget.boxHeight));
          item.insert_or_assign("rotation", static_cast<double>(widget.rotationRad));
          if (widget.flipX) {
            item.insert_or_assign("flip_x", true);
          }
          if (widget.flipY) {
            item.insert_or_assign("flip_y", true);
          }
          item.insert_or_assign("enabled", widget.enabled);

          toml::table settings;
          std::vector<std::string> keys;
          keys.reserve(widget.settings.size());
          for (const auto& [key, value] : widget.settings) {
            (void)value;
            keys.push_back(key);
          }
          std::ranges::sort(keys);
          for (const auto& key : keys) {
            insertWidgetSettingValue(settings, key, widget.settings.at(key));
          }
          item.insert_or_assign("settings", std::move(settings));
          widgetTable.insert_or_assign(widget.id, std::move(item));
        }
        table.insert_or_assign("widget_order", std::move(order));
        table.insert_or_assign("widget", std::move(widgetTable));
      }
      return table;
    }

    toml::table desktopWidgetsTable(const DesktopWidgetsConfig& desktopWidgets) {
      return widgetsPlacementTable(
          desktopWidgets.enabled, desktopWidgets.schemaVersion, desktopWidgets.grid, desktopWidgets.widgets
      );
    }

  } // namespace

  toml::table serialize(const Config& config) {
    toml::table root;

    root.insert_or_assign("shell", schema::writeTable(config.shell, schema::shellSchema()));
    root.insert_or_assign("wallpaper", schema::writeTable(config.wallpaper, schema::wallpaperSchema()));
    root.insert_or_assign("theme", schema::writeTable(config.theme, schema::themeSchema()));

    root.insert_or_assign("backdrop", schema::writeTable(config.backdrop, schema::backdropSchema()));

    root.insert_or_assign("lockscreen", schema::writeTable(config.lockscreen, schema::lockscreenSchema()));
    root.insert_or_assign(
        "lockscreen_widgets",
        widgetsPlacementTable(
            config.lockscreenWidgets.enabled, config.lockscreenWidgets.schemaVersion, config.lockscreenWidgets.grid,
            config.lockscreenWidgets.widgets
        )
    );

    root.insert_or_assign("notification", schema::writeTable(config.notification, schema::notificationSchema()));

    root.insert_or_assign("osd", schema::writeTable(config.osd, schema::osdSchema()));

    root.insert_or_assign("system", schema::writeTable(config.system, schema::systemSchema()));

    root.insert_or_assign("weather", schema::writeTable(config.weather, schema::weatherSchema()));
    root.insert_or_assign("calendar", schema::writeTable(config.calendar, schema::calendarSchema()));
    root.insert_or_assign("audio", schema::writeTable(config.audio, schema::audioSchema()));

    root.insert_or_assign("brightness", schema::writeTable(config.brightness, schema::brightnessSchema()));
    root.insert_or_assign("battery", schema::writeTable(config.battery, schema::batterySchema()));

    root.insert_or_assign("nightlight", schema::writeTable(config.nightlight, schema::nightlightSchema()));
    root.insert_or_assign("location", schema::writeTable(config.location, schema::locationSchema()));

    root.insert_or_assign("idle", schema::writeTable(config.idle, schema::idleSchema()));

    root.insert_or_assign("keybinds", schema::writeTable(config.keybinds, schema::keybindsSchema()));

    toml::table barRoot;
    toml::array barOrder;
    for (const auto& bar : config.bars) {
      if (bar.name.empty()) {
        continue;
      }
      barOrder.push_back(bar.name);
      barRoot.insert_or_assign(bar.name, barConfigTable(bar));
    }
    barRoot.insert_or_assign("order", std::move(barOrder));
    root.insert_or_assign("bar", std::move(barRoot));

    root.insert_or_assign("dock", schema::writeTable(config.dock, schema::dockSchema()));
    root.insert_or_assign("desktop_widgets", desktopWidgetsTable(config.desktopWidgets));

    toml::table widgetRoot;
    std::vector<std::string> widgetNames;
    widgetNames.reserve(config.widgets.size());
    for (const auto& [name, widget] : config.widgets) {
      (void)widget;
      widgetNames.push_back(name);
    }
    std::ranges::sort(widgetNames);
    for (const auto& name : widgetNames) {
      widgetRoot.insert_or_assign(name, widgetConfigTable(config.widgets.at(name)));
    }
    root.insert_or_assign("widget", std::move(widgetRoot));

    root.insert_or_assign("control_center", schema::writeTable(config.controlCenter, schema::controlCenterSchema()));

    root.insert_or_assign("plugins", schema::writeTable(config.plugins, schema::pluginsSchema()));

    root.insert_or_assign("hooks", schema::writeTable(config.hooks, schema::hooksSchema()));
    return root;
  }

} // namespace config_export
