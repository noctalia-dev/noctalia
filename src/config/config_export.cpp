#include "config/config_export.h"

#include "core/key_chord.h"
#include "render/core/color.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace config_export {
  namespace {

    int colorByteForExport(float value) {
      return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    }

    std::string colorToConfigString(const Color& color) {
      if (color.a >= 0.999f) {
        return formatRgbHex(color);
      }

      char buffer[16];
      std::snprintf(
          buffer, sizeof(buffer), "#%02X%02X%02X%02X", colorByteForExport(color.r), colorByteForExport(color.g),
          colorByteForExport(color.b), colorByteForExport(color.a)
      );
      return std::string(buffer);
    }

    std::string colorSpecToConfigString(const ColorSpec& spec) {
      if (spec.role.has_value()) {
        return std::string(colorRoleToken(*spec.role));
      }

      Color color = spec.fixed;
      color.a *= spec.alpha;
      return colorToConfigString(color);
    }

    toml::array stringArray(const std::vector<std::string>& values) {
      toml::array array;
      for (const auto& value : values) {
        array.push_back(value);
      }
      return array;
    }

    toml::array keyChordArray(const std::vector<KeyChord>& values) {
      toml::array array;
      for (const auto& value : values) {
        std::string serialized = keyChordToString(value);
        if (!serialized.empty()) {
          array.push_back(std::move(serialized));
        }
      }
      return array;
    }

    toml::array effectiveKeyChordArray(const std::vector<KeyChord>& values, KeybindAction action) {
      if (!values.empty()) {
        return keyChordArray(values);
      }
      return keyChordArray(defaultKeybindSet(action));
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
      std::sort(keys.begin(), keys.end());

      for (const auto& key : keys) {
        insertWidgetSettingValue(table, key, widget.settings.at(key));
      }
      return table;
    }

    toml::array shortcutArray(const std::vector<ShortcutConfig>& shortcuts) {
      toml::array array;
      for (const auto& shortcut : shortcuts) {
        if (shortcut.type.empty()) {
          continue;
        }
        toml::table item;
        item.insert_or_assign("type", shortcut.type);
        array.push_back(std::move(item));
      }
      return array;
    }

    toml::array sessionActionArray(const std::vector<SessionPanelActionConfig>& actions) {
      toml::array array;
      for (const auto& action : actions) {
        if (action.action.empty()) {
          continue;
        }
        toml::table item;
        item.insert_or_assign("action", action.action);
        item.insert_or_assign("enabled", action.enabled);
        item.insert_or_assign("command", action.command.value_or(""));
        item.insert_or_assign("label", action.label.value_or(""));
        item.insert_or_assign("glyph", action.glyph.value_or(""));
        item.insert_or_assign("variant", std::string(enumToKey(kSessionActionButtonVariants, action.variant)));
        item.insert_or_assign(
            "shortcut", action.shortcut.has_value() ? keyChordToString(*action.shortcut) : std::string{}
        );
        array.push_back(std::move(item));
      }
      return array;
    }

    toml::array wallpaperTransitionArray(const std::vector<WallpaperTransition>& transitions) {
      toml::array array;
      for (const auto transition : transitions) {
        const std::string_view key = enumToKey(kWallpaperTransitions, transition);
        if (!key.empty()) {
          array.push_back(std::string(key));
        }
      }
      return array;
    }

    void insertBarFields(toml::table& table, const BarConfig& bar, bool includePosition) {
      if (includePosition) {
        table.insert_or_assign("position", bar.position);
      }
      table.insert_or_assign("enabled", bar.enabled);
      table.insert_or_assign("auto_hide", bar.autoHide);
      table.insert_or_assign("reserve_space", bar.reserveSpace);
      table.insert_or_assign("thickness", static_cast<std::int64_t>(bar.thickness));
      table.insert_or_assign("background_opacity", static_cast<double>(bar.backgroundOpacity));
      table.insert_or_assign("border", colorSpecToConfigString(bar.border));
      table.insert_or_assign("border_width", static_cast<double>(bar.borderWidth));
      table.insert_or_assign("radius", static_cast<std::int64_t>(bar.radius));
      table.insert_or_assign("radius_top_left", static_cast<std::int64_t>(bar.radiusTopLeft));
      table.insert_or_assign("radius_top_right", static_cast<std::int64_t>(bar.radiusTopRight));
      table.insert_or_assign("radius_bottom_left", static_cast<std::int64_t>(bar.radiusBottomLeft));
      table.insert_or_assign("radius_bottom_right", static_cast<std::int64_t>(bar.radiusBottomRight));
      table.insert_or_assign("margin_ends", static_cast<std::int64_t>(bar.marginEnds));
      table.insert_or_assign("margin_edge", static_cast<std::int64_t>(bar.marginEdge));
      table.insert_or_assign("padding", static_cast<std::int64_t>(bar.padding));
      table.insert_or_assign("widget_spacing", static_cast<std::int64_t>(bar.widgetSpacing));
      table.insert_or_assign("shadow", bar.shadow);
      table.insert_or_assign("contact_shadow", bar.contactShadow);
      table.insert_or_assign("scale", static_cast<double>(bar.scale));
      table.insert_or_assign("font_weight", static_cast<std::int64_t>(bar.fontWeight));
      table.insert_or_assign("start", stringArray(bar.startWidgets));
      table.insert_or_assign("center", stringArray(bar.centerWidgets));
      table.insert_or_assign("end", stringArray(bar.endWidgets));
      table.insert_or_assign("capsule", bar.widgetCapsuleDefault);
      table.insert_or_assign("capsule_fill", colorSpecToConfigString(bar.widgetCapsuleFill));
      if (bar.widgetCapsuleForeground.has_value()) {
        table.insert_or_assign("capsule_foreground", colorSpecToConfigString(*bar.widgetCapsuleForeground));
      }
      if (bar.widgetColor.has_value()) {
        table.insert_or_assign("color", colorSpecToConfigString(*bar.widgetColor));
      }
      table.insert_or_assign("capsule_groups", stringArray(bar.widgetCapsuleGroups));
      table.insert_or_assign("capsule_padding", static_cast<double>(bar.widgetCapsulePadding));
      if (bar.widgetCapsuleRadius.has_value()) {
        table.insert_or_assign("capsule_radius", *bar.widgetCapsuleRadius);
      }
      table.insert_or_assign("capsule_opacity", static_cast<double>(bar.widgetCapsuleOpacity));
      if (bar.widgetCapsuleBorderSpecified) {
        table.insert_or_assign(
            "capsule_border",
            bar.widgetCapsuleBorder.has_value() ? colorSpecToConfigString(*bar.widgetCapsuleBorder) : std::string{}
        );
      }
    }

    BarConfig applyMonitorOverride(const BarConfig& base, const BarMonitorOverride& ovr) {
      BarConfig resolved = base;
      if (ovr.enabled)
        resolved.enabled = *ovr.enabled;
      if (ovr.autoHide)
        resolved.autoHide = *ovr.autoHide;
      if (ovr.reserveSpace)
        resolved.reserveSpace = *ovr.reserveSpace;
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
      if (ovr.padding)
        resolved.padding = *ovr.padding;
      if (ovr.widgetSpacing)
        resolved.widgetSpacing = *ovr.widgetSpacing;
      if (ovr.shadow)
        resolved.shadow = *ovr.shadow;
      if (ovr.contactShadow)
        resolved.contactShadow = *ovr.contactShadow;
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
      if (ovr.widgetCapsuleGroups)
        resolved.widgetCapsuleGroups = *ovr.widgetCapsuleGroups;
      if (ovr.widgetCapsulePadding)
        resolved.widgetCapsulePadding = static_cast<float>(*ovr.widgetCapsulePadding);
      if (ovr.widgetCapsuleRadius.has_value()) {
        resolved.widgetCapsuleRadius = *ovr.widgetCapsuleRadius;
      }
      if (ovr.widgetCapsuleOpacity)
        resolved.widgetCapsuleOpacity = static_cast<float>(*ovr.widgetCapsuleOpacity);
      return resolved;
    }

    toml::table barConfigTable(const BarConfig& bar) {
      toml::table table;
      insertBarFields(table, bar, true);

      if (!bar.monitorOverrides.empty()) {
        toml::table monitors;
        for (const auto& ovr : bar.monitorOverrides) {
          if (ovr.match.empty()) {
            continue;
          }
          toml::table monitor = toml::table{};
          monitor.insert_or_assign("match", ovr.match);
          insertBarFields(monitor, applyMonitorOverride(bar, ovr), false);
          monitors.insert_or_assign(ovr.match, std::move(monitor));
        }
        table.insert_or_assign("monitor", std::move(monitors));
      }
      return table;
    }

    toml::table shellTable(const ShellConfig& shell) {
      toml::table table;
      table.insert_or_assign("ui_scale", static_cast<double>(shell.uiScale));
      table.insert_or_assign("corner_radius_scale", static_cast<double>(shell.cornerRadiusScale));
      table.insert_or_assign("font_family", shell.fontFamily);
      if (!shell.lang.empty()) {
        table.insert_or_assign("lang", shell.lang);
      }
      table.insert_or_assign("time_format", shell.timeFormat);
      table.insert_or_assign("date_format", shell.dateFormat);
      table.insert_or_assign("offline_mode", shell.offlineMode);
      table.insert_or_assign("telemetry_enabled", shell.telemetryEnabled);
      table.insert_or_assign("niri_overview_type_to_launch_enabled", shell.niriOverviewTypeToLaunchEnabled);
      table.insert_or_assign("polkit_agent", shell.polkitAgent);
      table.insert_or_assign("password_style", std::string(enumToKey(kPasswordMaskStyles, shell.passwordMaskStyle)));
      table.insert_or_assign("settings_show_advanced", shell.settingsShowAdvanced);
      table.insert_or_assign("middle_click_opens_widget_settings", shell.middleClickOpensWidgetSettings);
      table.insert_or_assign("show_location", shell.showLocation);
      table.insert_or_assign("launch_apps_as_systemd_services", shell.launchAppsAsSystemdServices);
      table.insert_or_assign("clipboard_enabled", shell.clipboardEnabled);
      table.insert_or_assign(
          "clipboard_history_max_entries", static_cast<std::int64_t>(shell.clipboardHistoryMaxEntries)
      );
      table.insert_or_assign("screen_time_enabled", shell.screenTimeEnabled);
      table.insert_or_assign(
          "clipboard_auto_paste", std::string(enumToKey(kClipboardAutoPasteModes, shell.clipboardAutoPaste))
      );
      table.insert_or_assign("clipboard_image_action_command", shell.clipboardImageActionCommand);
      if (!shell.avatarPath.empty()) {
        table.insert_or_assign("avatar_path", shell.avatarPath);
      }

      toml::table animation;
      animation.insert_or_assign("enabled", shell.animation.enabled);
      animation.insert_or_assign("speed", static_cast<double>(shell.animation.speed));
      table.insert_or_assign("animation", std::move(animation));

      toml::table shadow;
      shadow.insert_or_assign("blur", static_cast<std::int64_t>(shell.shadow.blur));
      shadow.insert_or_assign("offset_x", static_cast<std::int64_t>(shell.shadow.offsetX));
      shadow.insert_or_assign("offset_y", static_cast<std::int64_t>(shell.shadow.offsetY));
      shadow.insert_or_assign("alpha", static_cast<double>(shell.shadow.alpha));
      table.insert_or_assign("shadow", std::move(shadow));

      toml::table panel;
      panel.insert_or_assign("background_blur", shell.panel.backgroundBlur);
      panel.insert_or_assign(
          "transparency_mode", std::string(enumToKey(kPanelTransparencyModes, shell.panel.transparencyMode))
      );
      panel.insert_or_assign("borders", shell.panel.borders);
      panel.insert_or_assign("shadow", shell.panel.shadow);
      panel.insert_or_assign(
          "launcher_placement", std::string(enumToKey(kPanelPlacements, shell.panel.launcherPlacement))
      );
      panel.insert_or_assign(
          "clipboard_placement", std::string(enumToKey(kPanelPlacements, shell.panel.clipboardPlacement))
      );
      panel.insert_or_assign(
          "control_center_placement", std::string(enumToKey(kPanelPlacements, shell.panel.controlCenterPlacement))
      );
      panel.insert_or_assign(
          "wallpaper_placement", std::string(enumToKey(kPanelPlacements, shell.panel.wallpaperPlacement))
      );
      panel.insert_or_assign(
          "session_placement", std::string(enumToKey(kPanelPlacements, shell.panel.sessionPlacement))
      );
      panel.insert_or_assign("open_near_click_control_center", shell.panel.openNearClickControlCenter);
      panel.insert_or_assign("open_near_click_launcher", shell.panel.openNearClickLauncher);
      panel.insert_or_assign("open_near_click_clipboard", shell.panel.openNearClickClipboard);
      panel.insert_or_assign("open_near_click_wallpaper", shell.panel.openNearClickWallpaper);
      panel.insert_or_assign("open_near_click_session", shell.panel.openNearClickSession);
      panel.insert_or_assign("launcher_categories", shell.panel.launcherCategories);
      table.insert_or_assign("panel", std::move(panel));

      toml::table screenCorners;
      screenCorners.insert_or_assign("enabled", shell.screenCorners.enabled);
      screenCorners.insert_or_assign("size", static_cast<std::int64_t>(shell.screenCorners.size));
      table.insert_or_assign("screen_corners", std::move(screenCorners));

      toml::table mpris;
      mpris.insert_or_assign("blacklist", stringArray(shell.mpris.blacklist));
      table.insert_or_assign("mpris", std::move(mpris));

      toml::table session;
      session.insert_or_assign("actions", sessionActionArray(shell.session.actions));
      table.insert_or_assign("session", std::move(session));
      return table;
    }

    toml::table themeTable(const ThemeConfig& theme) {
      toml::table table;
      table.insert_or_assign("source", std::string(enumToKey(kPaletteSources, theme.source)));
      table.insert_or_assign("builtin", theme.builtinPalette);
      table.insert_or_assign("community_palette", theme.communityPalette);
      table.insert_or_assign("custom_palette", theme.customPalette);
      table.insert_or_assign("wallpaper_scheme", theme.wallpaperScheme);
      table.insert_or_assign("mode", std::string(enumToKey(kThemeModes, theme.mode)));

      toml::table templates;
      templates.insert_or_assign("enable_builtin_templates", theme.templates.enableBuiltinTemplates);
      templates.insert_or_assign("builtin_ids", stringArray(theme.templates.builtinIds));
      templates.insert_or_assign("enable_community_templates", theme.templates.enableCommunityTemplates);
      templates.insert_or_assign("community_ids", stringArray(theme.templates.communityIds));

      if (!theme.templates.customColors.empty()) {
        toml::table customColors;
        for (const auto& color : theme.templates.customColors) {
          toml::table colorTable;
          colorTable.insert_or_assign("color", color.color);
          colorTable.insert_or_assign("blend", color.blend);
          customColors.insert_or_assign(color.name, std::move(colorTable));
        }
        templates.insert_or_assign("custom_colors", std::move(customColors));
      }

      if (!theme.templates.userTemplates.empty()) {
        toml::table userTemplates;
        for (const auto& tmpl : theme.templates.userTemplates) {
          if (tmpl.id.empty()) {
            continue;
          }
          toml::table item;
          item.insert_or_assign("enabled", tmpl.enabled);
          item.insert_or_assign("input_path", tmpl.inputPath);
          if (tmpl.inputPathModes.has_value()) {
            toml::table modes;
            modes.insert_or_assign("dark", tmpl.inputPathModes->dark);
            modes.insert_or_assign("light", tmpl.inputPathModes->light);
            item.insert_or_assign("input_path_modes", std::move(modes));
          }
          item.insert_or_assign("output_path", stringArray(tmpl.outputPaths));
          item.insert_or_assign("output_path_dynamic", tmpl.outputPathDynamic);
          item.insert_or_assign("compare_to", tmpl.compareTo);
          if (!tmpl.colorsToCompare.empty()) {
            toml::array colors;
            for (const auto& color : tmpl.colorsToCompare) {
              toml::table colorTable;
              colorTable.insert_or_assign("name", color.name);
              colorTable.insert_or_assign("color", color.color);
              colors.push_back(std::move(colorTable));
            }
            item.insert_or_assign("colors_to_compare", std::move(colors));
          }
          item.insert_or_assign("pre_hook", tmpl.preHook);
          item.insert_or_assign("post_hook", tmpl.postHook);
          item.insert_or_assign("index", static_cast<std::int64_t>(tmpl.index));
          userTemplates.insert_or_assign(tmpl.id, std::move(item));
        }
        templates.insert_or_assign("user", std::move(userTemplates));
      }

      table.insert_or_assign("templates", std::move(templates));
      return table;
    }

    toml::table wallpaperTable(const WallpaperConfig& wallpaper) {
      toml::table table;
      table.insert_or_assign("enabled", wallpaper.enabled);
      table.insert_or_assign("fill_mode", std::string(enumToKey(kWallpaperFillModes, wallpaper.fillMode)));
      table.insert_or_assign(
          "fill_color", wallpaper.fillColor.has_value() ? colorSpecToConfigString(*wallpaper.fillColor) : std::string{}
      );
      table.insert_or_assign("transition", wallpaperTransitionArray(wallpaper.transitions));
      table.insert_or_assign("transition_duration", static_cast<double>(wallpaper.transitionDurationMs));
      table.insert_or_assign("edge_smoothness", static_cast<double>(wallpaper.edgeSmoothness));
      table.insert_or_assign("directory", wallpaper.directory);
      table.insert_or_assign("directory_light", wallpaper.directoryLight);
      table.insert_or_assign("directory_dark", wallpaper.directoryDark);
      table.insert_or_assign("per_monitor_directories", wallpaper.perMonitorDirectories);

      toml::table automation;
      automation.insert_or_assign("enabled", wallpaper.automation.enabled);
      automation.insert_or_assign("interval_minutes", static_cast<std::int64_t>(wallpaper.automation.intervalMinutes));
      automation.insert_or_assign(
          "order", std::string(enumToKey(kWallpaperAutomationOrders, wallpaper.automation.order))
      );
      automation.insert_or_assign("recursive", wallpaper.automation.recursive);
      table.insert_or_assign("automation", std::move(automation));

      if (!wallpaper.monitorOverrides.empty()) {
        toml::table monitors;
        for (const auto& ovr : wallpaper.monitorOverrides) {
          if (ovr.match.empty()) {
            continue;
          }
          toml::table monitor;
          monitor.insert_or_assign("match", ovr.match);
          if (ovr.enabled.has_value()) {
            monitor.insert_or_assign("enabled", *ovr.enabled);
          }
          if (ovr.fillColor.has_value()) {
            monitor.insert_or_assign("fill_color", colorSpecToConfigString(*ovr.fillColor));
          }
          if (ovr.directory.has_value()) {
            monitor.insert_or_assign("directory", *ovr.directory);
          }
          if (ovr.directoryLight.has_value()) {
            monitor.insert_or_assign("directory_light", *ovr.directoryLight);
          }
          if (ovr.directoryDark.has_value()) {
            monitor.insert_or_assign("directory_dark", *ovr.directoryDark);
          }
          monitors.insert_or_assign(ovr.match, std::move(monitor));
        }
        table.insert_or_assign("monitor", std::move(monitors));
      }
      return table;
    }

    toml::table dockTable(const DockConfig& dock) {
      toml::table table;
      table.insert_or_assign("enabled", dock.enabled);
      table.insert_or_assign("position", dock.position);
      table.insert_or_assign("active_monitor_only", dock.activeMonitorOnly);
      table.insert_or_assign("icon_size", static_cast<std::int64_t>(dock.iconSize));
      table.insert_or_assign("padding", static_cast<std::int64_t>(dock.padding));
      table.insert_or_assign("item_spacing", static_cast<std::int64_t>(dock.itemSpacing));
      table.insert_or_assign("background_opacity", static_cast<double>(dock.backgroundOpacity));
      table.insert_or_assign("radius", static_cast<std::int64_t>(dock.radius));
      table.insert_or_assign("radius_top_left", static_cast<std::int64_t>(dock.radiusTopLeft));
      table.insert_or_assign("radius_top_right", static_cast<std::int64_t>(dock.radiusTopRight));
      table.insert_or_assign("radius_bottom_left", static_cast<std::int64_t>(dock.radiusBottomLeft));
      table.insert_or_assign("radius_bottom_right", static_cast<std::int64_t>(dock.radiusBottomRight));
      table.insert_or_assign("margin_ends", static_cast<std::int64_t>(dock.marginEnds));
      table.insert_or_assign("margin_edge", static_cast<std::int64_t>(dock.marginEdge));
      table.insert_or_assign("shadow", dock.shadow);
      table.insert_or_assign("show_running", dock.showRunning);
      table.insert_or_assign("auto_hide", dock.autoHide);
      table.insert_or_assign("reserve_space", dock.reserveSpace);
      table.insert_or_assign("active_scale", static_cast<double>(dock.activeScale));
      table.insert_or_assign("inactive_scale", static_cast<double>(dock.inactiveScale));
      table.insert_or_assign("active_opacity", static_cast<double>(dock.activeOpacity));
      table.insert_or_assign("inactive_opacity", static_cast<double>(dock.inactiveOpacity));
      table.insert_or_assign("show_dots", dock.showDots);
      table.insert_or_assign("show_instance_count", dock.showInstanceCount);
      table.insert_or_assign("launcher_position", dock.launcherPosition);
      table.insert_or_assign("launcher_icon", dock.launcherIcon);
      table.insert_or_assign("pinned", stringArray(dock.pinned));
      table.insert_or_assign("monitors", stringArray(dock.monitors));
      return table;
    }

    toml::table desktopWidgetsTable(const DesktopWidgetsConfig& desktopWidgets) {
      toml::table table;
      table.insert_or_assign("enabled", desktopWidgets.enabled);
      table.insert_or_assign("schema_version", static_cast<std::int64_t>(desktopWidgets.schemaVersion));

      toml::table grid;
      grid.insert_or_assign("visible", desktopWidgets.grid.visible);
      grid.insert_or_assign("cell_size", static_cast<std::int64_t>(desktopWidgets.grid.cellSize));
      grid.insert_or_assign("major_interval", static_cast<std::int64_t>(desktopWidgets.grid.majorInterval));
      table.insert_or_assign("grid", std::move(grid));

      if (!desktopWidgets.widgets.empty()) {
        toml::array order;
        toml::table widgets;
        for (const auto& widget : desktopWidgets.widgets) {
          if (widget.id.empty()) {
            continue;
          }
          order.push_back(widget.id);
          toml::table item;
          item.insert_or_assign("type", widget.type);
          item.insert_or_assign("output", widget.outputName);
          item.insert_or_assign("cx", static_cast<double>(widget.cx));
          item.insert_or_assign("cy", static_cast<double>(widget.cy));
          item.insert_or_assign("scale", static_cast<double>(widget.scale));
          item.insert_or_assign("rotation", static_cast<double>(widget.rotationRad));
          item.insert_or_assign("enabled", widget.enabled);

          toml::table settings;
          std::vector<std::string> keys;
          keys.reserve(widget.settings.size());
          for (const auto& [key, value] : widget.settings) {
            (void)value;
            keys.push_back(key);
          }
          std::sort(keys.begin(), keys.end());
          for (const auto& key : keys) {
            insertWidgetSettingValue(settings, key, widget.settings.at(key));
          }
          item.insert_or_assign("settings", std::move(settings));
          widgets.insert_or_assign(widget.id, std::move(item));
        }
        table.insert_or_assign("widget_order", std::move(order));
        table.insert_or_assign("widget", std::move(widgets));
      }
      return table;
    }

    toml::table notificationTable(const NotificationConfig& notification) {
      toml::table table;
      table.insert_or_assign("enable_daemon", notification.enableDaemon);
      table.insert_or_assign("position", notification.position);
      table.insert_or_assign("layer", notification.layer);
      table.insert_or_assign("scale", static_cast<double>(notification.scale));
      table.insert_or_assign("background_opacity", static_cast<double>(notification.backgroundOpacity));
      table.insert_or_assign("offset_x", static_cast<std::int64_t>(notification.offsetX));
      table.insert_or_assign("offset_y", static_cast<std::int64_t>(notification.offsetY));
      table.insert_or_assign("monitors", stringArray(notification.monitors));
      return table;
    }

    toml::table brightnessTable(const BrightnessConfig& brightness) {
      toml::table table;
      table.insert_or_assign("enable_ddcutil", brightness.enableDdcutil);
      table.insert_or_assign("ignore_mmids", stringArray(brightness.ddcutilIgnoreMmids));

      if (!brightness.monitorOverrides.empty()) {
        toml::table monitors;
        for (const auto& ovr : brightness.monitorOverrides) {
          if (ovr.match.empty()) {
            continue;
          }
          toml::table monitor;
          monitor.insert_or_assign("match", ovr.match);
          if (ovr.backend.has_value()) {
            monitor.insert_or_assign("backend", std::string(enumToKey(kBrightnessBackendPreferences, *ovr.backend)));
          }
          monitors.insert_or_assign(ovr.match, std::move(monitor));
        }
        table.insert_or_assign("monitor", std::move(monitors));
      }
      return table;
    }

    toml::table idleTable(const IdleConfig& idle) {
      toml::table table;
      table.insert_or_assign("pre_action_fade_seconds", static_cast<double>(idle.preActionFadeSeconds));

      toml::array order;
      toml::table behaviors;
      for (const auto& behavior : idle.behaviors) {
        if (behavior.name.empty()) {
          continue;
        }
        order.push_back(behavior.name);
        toml::table item;
        item.insert_or_assign("enabled", behavior.enabled);
        item.insert_or_assign("timeout", static_cast<std::int64_t>(behavior.timeoutSeconds));
        item.insert_or_assign("action", behavior.action);
        item.insert_or_assign("command", behavior.command);
        item.insert_or_assign("resume_command", behavior.resumeCommand);
        item.insert_or_assign("lock_before_suspend", behavior.lockBeforeSuspend);
        behaviors.insert_or_assign(behavior.name, std::move(item));
      }
      table.insert_or_assign("behavior_order", std::move(order));
      table.insert_or_assign("behavior", std::move(behaviors));
      return table;
    }

    toml::table hooksTable(const HooksConfig& hooks) {
      toml::table table;
      table.insert_or_assign(
          "battery_low_percent_threshold", static_cast<std::int64_t>(hooks.batteryLowPercentThreshold)
      );
      for (std::size_t i = 0; i < static_cast<std::size_t>(HookKind::Count); ++i) {
        const auto kind = static_cast<HookKind>(i);
        table.insert_or_assign(hookKindKey(kind), stringArray(hooks.commands[i]));
      }
      return table;
    }

  } // namespace

  toml::table configToToml(const Config& config) {
    toml::table root;

    root.insert_or_assign("shell", shellTable(config.shell));
    root.insert_or_assign("wallpaper", wallpaperTable(config.wallpaper));
    root.insert_or_assign("theme", themeTable(config.theme));

    toml::table backdrop;
    backdrop.insert_or_assign("enabled", config.backdrop.enabled);
    backdrop.insert_or_assign("blur_intensity", static_cast<double>(config.backdrop.blurIntensity));
    backdrop.insert_or_assign("tint_intensity", static_cast<double>(config.backdrop.tintIntensity));
    root.insert_or_assign("backdrop", std::move(backdrop));

    root.insert_or_assign("notification", notificationTable(config.notification));

    toml::table osd;
    osd.insert_or_assign("position", config.osd.position);
    osd.insert_or_assign("orientation", config.osd.orientation);
    osd.insert_or_assign("scale", static_cast<double>(config.osd.scale));
    osd.insert_or_assign("lock_keys", config.osd.lockKeys);
    osd.insert_or_assign("keyboard_layout", config.osd.keyboardLayout);
    root.insert_or_assign("osd", std::move(osd));

    toml::table system;
    toml::table monitor;
    const auto& mon = config.system.monitor;
    monitor.insert_or_assign("enabled", mon.enabled);
    monitor.insert_or_assign("cpu_poll_seconds", static_cast<double>(mon.cpuPollSeconds));
    monitor.insert_or_assign("gpu_poll_seconds", static_cast<double>(mon.gpuPollSeconds));
    monitor.insert_or_assign("memory_poll_seconds", static_cast<double>(mon.memoryPollSeconds));
    monitor.insert_or_assign("network_poll_seconds", static_cast<double>(mon.networkPollSeconds));
    monitor.insert_or_assign("disk_poll_seconds", static_cast<double>(mon.diskPollSeconds));
    system.insert_or_assign("monitor", std::move(monitor));
    root.insert_or_assign("system", std::move(system));

    toml::table weather;
    weather.insert_or_assign("enabled", config.weather.enabled);
    weather.insert_or_assign("auto_locate", config.weather.autoLocate);
    weather.insert_or_assign("effects", config.weather.effects);
    weather.insert_or_assign("address", config.weather.address);
    weather.insert_or_assign("refresh_minutes", static_cast<std::int64_t>(config.weather.refreshMinutes));
    weather.insert_or_assign("unit", config.weather.unit);
    root.insert_or_assign("weather", std::move(weather));

    toml::table audio;
    audio.insert_or_assign("enable_overdrive", config.audio.enableOverdrive);
    audio.insert_or_assign("enable_sounds", config.audio.enableSounds);
    audio.insert_or_assign("sound_volume", static_cast<double>(config.audio.soundVolume));
    audio.insert_or_assign("volume_change_sound", config.audio.volumeChangeSound);
    audio.insert_or_assign("notification_sound", config.audio.notificationSound);
    root.insert_or_assign("audio", std::move(audio));

    root.insert_or_assign("brightness", brightnessTable(config.brightness));

    toml::table nightlight;
    nightlight.insert_or_assign("enabled", config.nightlight.enabled);
    nightlight.insert_or_assign("force", config.nightlight.force);
    nightlight.insert_or_assign("use_weather_location", config.nightlight.useWeatherLocation);
    nightlight.insert_or_assign("start_time", config.nightlight.startTime);
    nightlight.insert_or_assign("stop_time", config.nightlight.stopTime);
    if (config.nightlight.latitude.has_value()) {
      nightlight.insert_or_assign("latitude", *config.nightlight.latitude);
    }
    if (config.nightlight.longitude.has_value()) {
      nightlight.insert_or_assign("longitude", *config.nightlight.longitude);
    }
    nightlight.insert_or_assign("temperature_day", static_cast<std::int64_t>(config.nightlight.dayTemperature));
    nightlight.insert_or_assign("temperature_night", static_cast<std::int64_t>(config.nightlight.nightTemperature));
    root.insert_or_assign("nightlight", std::move(nightlight));

    root.insert_or_assign("idle", idleTable(config.idle));

    toml::table keybinds;
    keybinds.insert_or_assign("validate", effectiveKeyChordArray(config.keybinds.validate, KeybindAction::Validate));
    keybinds.insert_or_assign("cancel", effectiveKeyChordArray(config.keybinds.cancel, KeybindAction::Cancel));
    keybinds.insert_or_assign("left", effectiveKeyChordArray(config.keybinds.left, KeybindAction::Left));
    keybinds.insert_or_assign("right", effectiveKeyChordArray(config.keybinds.right, KeybindAction::Right));
    keybinds.insert_or_assign("up", effectiveKeyChordArray(config.keybinds.up, KeybindAction::Up));
    keybinds.insert_or_assign("down", effectiveKeyChordArray(config.keybinds.down, KeybindAction::Down));
    root.insert_or_assign("keybinds", std::move(keybinds));

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

    root.insert_or_assign("dock", dockTable(config.dock));
    root.insert_or_assign("desktop_widgets", desktopWidgetsTable(config.desktopWidgets));

    toml::table widgetRoot;
    std::vector<std::string> widgetNames;
    widgetNames.reserve(config.widgets.size());
    for (const auto& [name, widget] : config.widgets) {
      (void)widget;
      widgetNames.push_back(name);
    }
    std::sort(widgetNames.begin(), widgetNames.end());
    for (const auto& name : widgetNames) {
      widgetRoot.insert_or_assign(name, widgetConfigTable(config.widgets.at(name)));
    }
    root.insert_or_assign("widget", std::move(widgetRoot));

    toml::table controlCenter;
    controlCenter.insert_or_assign("compact", config.controlCenter.compact);
    controlCenter.insert_or_assign("shortcuts", shortcutArray(config.controlCenter.shortcuts));
    root.insert_or_assign("control_center", std::move(controlCenter));

    root.insert_or_assign("hooks", hooksTable(config.hooks));
    return root;
  }

} // namespace config_export
