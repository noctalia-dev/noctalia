#include "config/schema/config_schema.h"

#include "config/schema/engine.h"
#include "config/schema/ranges.h"
#include "core/key_chord.h"
#include "util/file_utils.h"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <unordered_map>

namespace noctalia::config::schema {

  const Schema<AudioConfig>& audioSchema() {
    static const Schema<AudioConfig> s = {
        field(&AudioConfig::enableOverdrive, "enable_overdrive"),
        field(&AudioConfig::enableSounds, "enable_sounds"),
        field(&AudioConfig::soundVolume, "sound_volume", kUnitRange),
        field(&AudioConfig::volumeChangeSound, "volume_change_sound"),
        field(&AudioConfig::notificationSound, "notification_sound"),
    };
    return s;
  }

  const Schema<WeatherConfig>& weatherSchema() {
    static const Schema<WeatherConfig> s = {
        field(&WeatherConfig::enabled, "enabled"),
        field(&WeatherConfig::effects, "effects"),
        field(&WeatherConfig::refreshMinutes, "refresh_minutes", kRefreshMinutesRange),
        field(&WeatherConfig::unit, "unit"),
    };
    return s;
  }

  const Schema<OsdConfig>& osdSchema() {
    static const Schema<OsdConfig> s = {
        field(&OsdConfig::position, "position"),
        field(&OsdConfig::orientation, "orientation"),
        field(&OsdConfig::scale, "scale", kScaleRange),
        field(&OsdConfig::backgroundOpacity, "background_opacity", kUnitRange),
        field(&OsdConfig::offsetX, "offset_x", Range<std::int64_t>{0, std::nullopt}),
        field(&OsdConfig::offsetY, "offset_y", Range<std::int64_t>{0, std::nullopt}),
        field(&OsdConfig::monitors, "monitors"),
        field(&OsdConfig::lockKeys, "lock_keys"),
        field(&OsdConfig::keyboardLayout, "keyboard_layout"),
    };
    return s;
  }

  const Schema<BackdropConfig>& backdropSchema() {
    static const Schema<BackdropConfig> s = {
        field(&BackdropConfig::enabled, "enabled"),
        field(&BackdropConfig::blurIntensity, "blur_intensity", kUnitRange),
        field(&BackdropConfig::tintIntensity, "tint_intensity", kUnitRange),
    };
    return s;
  }

  const Schema<LockscreenConfig>& lockscreenSchema() {
    static const Schema<LockscreenConfig> s = {
        field(&LockscreenConfig::blurredDesktop, "blurred_desktop"),
        field(&LockscreenConfig::blurIntensity, "blur_intensity", kUnitRange),
        field(&LockscreenConfig::tintIntensity, "tint_intensity", kUnitRange),
        field(&LockscreenConfig::wallpaper, "wallpaper"),
    };
    return s;
  }

  namespace {
    // Poll-second floats are stored verbatim here; the [1,120]/disabled clamping
    // happens at consumption, not at parse time — so no Range is attached.
    const Schema<SystemConfig::MonitorConfig>& systemMonitorSchema() {
      static const Schema<SystemConfig::MonitorConfig> s = {
          field(&SystemConfig::MonitorConfig::enabled, "enabled"),
          field(&SystemConfig::MonitorConfig::cpuPollSeconds, "cpu_poll_seconds"),
          field(&SystemConfig::MonitorConfig::gpuPollSeconds, "gpu_poll_seconds"),
          field(&SystemConfig::MonitorConfig::memoryPollSeconds, "memory_poll_seconds"),
          field(&SystemConfig::MonitorConfig::networkPollSeconds, "network_poll_seconds"),
          field(&SystemConfig::MonitorConfig::diskPollSeconds, "disk_poll_seconds"),
          field(&SystemConfig::MonitorConfig::cpuUsageActivityThreshold, "cpu_usage_activity_threshold"),
          field(&SystemConfig::MonitorConfig::cpuUsageCriticalThreshold, "cpu_usage_critical_threshold"),
          field(&SystemConfig::MonitorConfig::cpuTempActivityThreshold, "cpu_temp_activity_threshold"),
          field(&SystemConfig::MonitorConfig::cpuTempCriticalThreshold, "cpu_temp_critical_threshold"),
          field(&SystemConfig::MonitorConfig::gpuTempActivityThreshold, "gpu_temp_activity_threshold"),
          field(&SystemConfig::MonitorConfig::gpuTempCriticalThreshold, "gpu_temp_critical_threshold"),
          field(&SystemConfig::MonitorConfig::gpuUsageActivityThreshold, "gpu_usage_activity_threshold"),
          field(&SystemConfig::MonitorConfig::gpuUsageCriticalThreshold, "gpu_usage_critical_threshold"),
          field(&SystemConfig::MonitorConfig::gpuVramActivityThreshold, "gpu_vram_activity_threshold"),
          field(&SystemConfig::MonitorConfig::gpuVramCriticalThreshold, "gpu_vram_critical_threshold"),
          field(&SystemConfig::MonitorConfig::ramPctActivityThreshold, "ram_pct_activity_threshold"),
          field(&SystemConfig::MonitorConfig::ramPctCriticalThreshold, "ram_pct_critical_threshold"),
          field(&SystemConfig::MonitorConfig::swapPctActivityThreshold, "swap_pct_activity_threshold"),
          field(&SystemConfig::MonitorConfig::swapPctCriticalThreshold, "swap_pct_critical_threshold"),
          field(&SystemConfig::MonitorConfig::diskPctActivityThreshold, "disk_pct_activity_threshold"),
          field(&SystemConfig::MonitorConfig::diskPctCriticalThreshold, "disk_pct_critical_threshold"),
          field(&SystemConfig::MonitorConfig::netRxActivityThreshold, "net_rx_activity_threshold"),
          field(&SystemConfig::MonitorConfig::netRxCriticalThreshold, "net_rx_critical_threshold"),
          field(&SystemConfig::MonitorConfig::netTxActivityThreshold, "net_tx_activity_threshold"),
          field(&SystemConfig::MonitorConfig::netTxCriticalThreshold, "net_tx_critical_threshold"),
      };
      return s;
    }
  } // namespace

  const Schema<SystemConfig>& systemSchema() {
    static const Schema<SystemConfig> s = {
        subTable(&SystemConfig::monitor, "monitor", systemMonitorSchema()),
    };
    return s;
  }

  const Schema<NightLightConfig>& nightlightSchema() {
    static const Schema<NightLightConfig> s = {
        field(&NightLightConfig::enabled, "enabled"),
        field(&NightLightConfig::force, "force"),
        field(&NightLightConfig::dayTemperature, "temperature_day", Range<std::int64_t>{1000, 25000}),
        field(&NightLightConfig::nightTemperature, "temperature_night", Range<std::int64_t>{1000, 25000}),
        // Day must lead night by at least the gap; pull night down, bumping day up
        // only if night would fall below the floor.
        finalize<NightLightConfig>([](NightLightConfig& nl, std::string_view path, Diagnostics& diag) {
          if (nl.dayTemperature - nl.nightTemperature >= NightLightConfig::kTemperatureGap) {
            return;
          }
          const std::int32_t origDay = nl.dayTemperature;
          const std::int32_t origNight = nl.nightTemperature;
          nl.nightTemperature = origDay - NightLightConfig::kTemperatureGap;
          if (nl.nightTemperature < NightLightConfig::kTemperatureMin) {
            nl.nightTemperature = NightLightConfig::kTemperatureMin;
            nl.dayTemperature = NightLightConfig::kTemperatureMin + NightLightConfig::kTemperatureGap;
          }
          diag.warn(
              std::string(path),
              std::format(
                  "temperatures must satisfy day > night (day={}K night={}K); adjusted to day={}K night={}K", origDay,
                  origNight, nl.dayTemperature, nl.nightTemperature
              )
          );
        }),
    };
    return s;
  }

  const Schema<LocationConfig>& locationSchema() {
    static const Schema<LocationConfig> s = {
        field(&LocationConfig::autoLocate, "auto_locate"), field(&LocationConfig::address, "address"),
        field(&LocationConfig::sunset, "sunset"),          field(&LocationConfig::sunrise, "sunrise"),
        field(&LocationConfig::latitude, "latitude"),      field(&LocationConfig::longitude, "longitude"),
    };
    return s;
  }

  const Schema<NotificationConfig>& notificationSchema() {
    static const Schema<NotificationConfig> s = {
        field(&NotificationConfig::enableDaemon, "enable_daemon"),
        field(&NotificationConfig::showAppName, "show_app_name"),
        field(&NotificationConfig::showActions, "show_actions"),
        field(&NotificationConfig::position, "position"),
        field(&NotificationConfig::layer, "layer"),
        field(&NotificationConfig::scale, "scale", kScaleRange),
        field(&NotificationConfig::backgroundOpacity, "background_opacity", kUnitRange),
        field(&NotificationConfig::offsetX, "offset_x"),
        field(&NotificationConfig::offsetY, "offset_y"),
        field(&NotificationConfig::monitors, "monitors"),
        field(&NotificationConfig::collapseOnDismiss, "collapse_on_dismiss"),
        field(&NotificationConfig::blacklist, "blacklist"),
        field(&NotificationConfig::blacklistAllowCritical, "blacklist_allow_critical"),
        field(&NotificationConfig::allowedUrgencies, "allowed_urgencies"),
    };
    return s;
  }

  const Schema<DockConfig>& dockSchema() {
    static const Schema<DockConfig> s = {
        field(&DockConfig::enabled, "enabled"),
        enumField(&DockConfig::position, "position", kDockEdges),
        field(&DockConfig::activeMonitorOnly, "active_monitor_only"),
        field(&DockConfig::iconSize, "icon_size", kDockIconSizeRange),
        field(&DockConfig::mainAxisPadding, "main_axis_padding", kDockPaddingRange),
        field(&DockConfig::crossAxisPadding, "cross_axis_padding", kDockPaddingRange),
        field(&DockConfig::itemSpacing, "item_spacing", kDockItemSpacingRange),
        field(&DockConfig::backgroundOpacity, "background_opacity", kUnitRange),
        // `radius` seeds all four corners; per-corner keys below override it.
        custom<DockConfig>(
            "radius",
            [](const toml::table& tbl, DockConfig& d, std::string_view, Diagnostics&) {
              if (auto v = tbl["radius"].value<std::int64_t>()) {
                const auto r = static_cast<std::int32_t>(applyRange<std::int64_t>(*v, kDockRadiusRange));
                d.radius = r;
                d.radiusTopLeft = r;
                d.radiusTopRight = r;
                d.radiusBottomLeft = r;
                d.radiusBottomRight = r;
              }
            },
            [](toml::table& tbl, const DockConfig& d) {
              tbl.insert_or_assign("radius", static_cast<std::int64_t>(d.radius));
            }
        ),
        field(&DockConfig::radiusTopLeft, "radius_top_left", kDockRadiusRange),
        field(&DockConfig::radiusTopRight, "radius_top_right", kDockRadiusRange),
        field(&DockConfig::radiusBottomLeft, "radius_bottom_left", kDockRadiusRange),
        field(&DockConfig::radiusBottomRight, "radius_bottom_right", kDockRadiusRange),
        field(&DockConfig::marginEnds, "margin_ends", kDockMarginEndsRange),
        field(&DockConfig::marginEdge, "margin_edge", kDockMarginEdgeRange),
        field(&DockConfig::shadow, "shadow"),
        field(&DockConfig::showRunning, "show_running"),
        field(&DockConfig::autoHide, "auto_hide"),
        field(&DockConfig::reserveSpace, "reserve_space"),
        field(&DockConfig::activeScale, "active_scale", kDockActiveScaleRange),
        field(&DockConfig::inactiveScale, "inactive_scale", kDockInactiveScaleRange),
        field(&DockConfig::magnification, "magnification"),
        field(&DockConfig::magnificationScale, "magnification_scale", kDockMagnificationScaleRange),
        field(&DockConfig::activeOpacity, "active_opacity", kUnitRange),
        field(&DockConfig::inactiveOpacity, "inactive_opacity", kUnitRange),
        field(&DockConfig::showDots, "show_dots"),
        field(&DockConfig::showInstanceCount, "show_instance_count"),
        enumField(&DockConfig::launcherPosition, "launcher_position", kDockLauncherPositions),
        field(&DockConfig::launcherIcon, "launcher_icon"),
        field(&DockConfig::pinned, "pinned"),
        field(&DockConfig::monitors, "monitors"),
    };
    return s;
  }

  namespace {
    const Schema<DesktopWidgetsGridState>& desktopWidgetsGridSchema() {
      static const Schema<DesktopWidgetsGridState> s = {
          field(&DesktopWidgetsGridState::visible, "visible"),
          field(&DesktopWidgetsGridState::cellSize, "cell_size", Range<std::int64_t>{8, 256}),
          field(&DesktopWidgetsGridState::majorInterval, "major_interval", Range<std::int64_t>{1, 16}),
      };
      return s;
    }
  } // namespace

  const Schema<DesktopWidgetsConfig>& desktopWidgetsSchema() {
    static const Schema<DesktopWidgetsConfig> s = {
        field(&DesktopWidgetsConfig::enabled, "enabled"),
        field(&DesktopWidgetsConfig::schemaVersion, "schema_version"),
        subTable(&DesktopWidgetsConfig::grid, "grid", desktopWidgetsGridSchema()),
    };
    return s;
  }

  const Schema<LockscreenWidgetsConfig>& lockscreenWidgetsSchema() {
    static const Schema<LockscreenWidgetsConfig> s = {
        field(&LockscreenWidgetsConfig::enabled, "enabled"),
        field(&LockscreenWidgetsConfig::schemaVersion, "schema_version"),
        subTable(&LockscreenWidgetsConfig::grid, "grid", desktopWidgetsGridSchema()),
    };
    return s;
  }

  namespace {
    const Schema<BrightnessMonitorOverride>& brightnessMonitorSchema() {
      static const Schema<BrightnessMonitorOverride> s = {
          field(&BrightnessMonitorOverride::match, "match"),
          optionalEnumField(&BrightnessMonitorOverride::backend, "backend", kBrightnessBackendPreferences),
      };
      return s;
    }

    const Schema<BatteryDeviceWarningThreshold>& batteryDeviceSchema() {
      static const Schema<BatteryDeviceWarningThreshold> s = {
          field(&BatteryDeviceWarningThreshold::warningThreshold, "warning_threshold", kBatteryWarningThresholdRange),
      };
      return s;
    }
  } // namespace

  const Schema<BrightnessConfig>& brightnessSchema() {
    static const Schema<BrightnessConfig> s = {
        field(&BrightnessConfig::enableDdcutil, "enable_ddcutil"),
        field(&BrightnessConfig::ddcutilIgnoreMmids, "ignore_mmids"),
        // Map key seeds `match`; an explicit `match` key inside overrides it.
        namedMap<BrightnessConfig, BrightnessMonitorOverride>(
            &BrightnessConfig::monitorOverrides, "monitor", brightnessMonitorSchema(),
            [](BrightnessMonitorOverride& o, std::string_view name) { o.match = std::string(name); },
            [](const BrightnessMonitorOverride& o) { return o.match; }
        ),
    };
    return s;
  }

  const Schema<BatteryConfig>& batterySchema() {
    static const Schema<BatteryConfig> s = {
        field(&BatteryConfig::warningThreshold, "warning_threshold", kBatteryWarningThresholdRange),
        // selector comes only from the map key; empty selectors are dropped.
        namedMap<BatteryConfig, BatteryDeviceWarningThreshold>(
            &BatteryConfig::deviceThresholds, "device", batteryDeviceSchema(),
            [](BatteryDeviceWarningThreshold& d, std::string_view name) { d.selector = std::string(name); },
            [](const BatteryDeviceWarningThreshold& d) { return d.selector; }, /*readSkipEmptyName=*/true
        ),
    };
    return s;
  }

  namespace {
    const Schema<ShortcutConfig>& shortcutSchema() {
      static const Schema<ShortcutConfig> s = {field(&ShortcutConfig::type, "type")};
      return s;
    }
  } // namespace

  const Schema<ControlCenterConfig>& controlCenterSchema() {
    static const Schema<ControlCenterConfig> s = {
        enumField(&ControlCenterConfig::sidebarMode, "sidebar", kControlCenterSidebarModes),
        enumField(&ControlCenterConfig::sidebarSectionMode, "sidebar_section", kControlCenterSidebarModes),
        arrayOf<ControlCenterConfig, ShortcutConfig>(
            &ControlCenterConfig::shortcuts, "shortcuts", shortcutSchema(),
            [](const ShortcutConfig& sc) { return !sc.type.empty(); }
        ),
    };
    return s;
  }

  namespace {
    // TOML key is "name" but the field is displayName.
    const Schema<CalendarConfig::Account>& calendarAccountSchema() {
      static const Schema<CalendarConfig::Account> s = {
          field(&CalendarConfig::Account::type, "type"),
          field(&CalendarConfig::Account::displayName, "name"),
          field(&CalendarConfig::Account::color, "color"),
          field(&CalendarConfig::Account::provider, "provider"),
          field(&CalendarConfig::Account::serverUrl, "server_url"),
          field(&CalendarConfig::Account::username, "username"),
          field(&CalendarConfig::Account::calendars, "calendars"),
          finalize<CalendarConfig::Account>([](CalendarConfig::Account& out, std::string_view parentPath,
                                               Diagnostics& diag) {
            if (out.type != "caldav") {
              return;
            }
            if (out.provider.empty()) {
              diag.error(
                  joinPath(parentPath, "provider"), "caldav accounts require provider = \"icloud\" or \"custom\""
              );
              return;
            }
            if (out.provider == "icloud") {
              if (!out.serverUrl.empty()) {
                diag.error(joinPath(parentPath, "server_url"), "icloud accounts use the built-in CalDAV server URL");
              }
              return;
            }
            if (out.provider == "custom") {
              if (out.serverUrl.empty()) {
                diag.error(joinPath(parentPath, "server_url"), "custom caldav accounts require server_url");
              }
              return;
            }
            diag.error(joinPath(parentPath, "provider"), "unknown caldav provider \"" + out.provider + "\"");
          }),
      };
      return s;
    }
  } // namespace

  namespace {
    // optional<ColorSpec> stored as a config string. alwaysEmit writes
    // string-or-empty unconditionally (wallpaper.fill_color); otherwise only when
    // set (monitor overrides). A present non-string value is a hard error.
    template <typename Struct>
    Field<Struct> colorSpecField(std::optional<ColorSpec> Struct::* member, std::string_view key, bool alwaysEmit) {
      return custom<Struct>(
          key,
          [member, key](const toml::table& tbl, Struct& out, std::string_view parentPath, Diagnostics&) {
            if (!tbl.contains(key)) {
              return;
            }
            auto v = tbl[key].value<std::string>();
            if (!v) {
              throw std::runtime_error(joinPath(parentPath, key) + ": expected string ColorSpec");
            }
            if (StringUtils::trim(*v).empty()) {
              out.*member = std::nullopt;
            } else {
              out.*member = colorSpecFromConfigString(*v, joinPath(parentPath, key));
            }
          },
          [member, key, alwaysEmit](toml::table& tbl, const Struct& in) {
            if ((in.*member).has_value()) {
              tbl.insert_or_assign(key, colorSpecToConfigString(*(in.*member)));
            } else if (alwaysEmit) {
              tbl.insert_or_assign(key, std::string{});
            }
          }
      );
    }

    // String holding a filesystem path: ~ and $VARS expand on read, emitted raw.
    template <typename Struct> Field<Struct> pathStringField(std::string Struct::* member, std::string_view key) {
      return custom<Struct>(
          key,
          [member, key](const toml::table& tbl, Struct& out, std::string_view, Diagnostics&) {
            if (auto v = tbl[key].value<std::string>()) {
              out.*member = v->empty() ? *v : FileUtils::expandUserPath(*v).string();
            }
          },
          [member, key](toml::table& tbl, const Struct& in) { tbl.insert_or_assign(key, in.*member); }
      );
    }

    template <typename Struct>
    Field<Struct> optionalPathStringField(std::optional<std::string> Struct::* member, std::string_view key) {
      return custom<Struct>(
          key,
          [member, key](const toml::table& tbl, Struct& out, std::string_view, Diagnostics&) {
            if (auto v = tbl[key].value<std::string>()) {
              out.*member = v->empty() ? *v : FileUtils::expandUserPath(*v).string();
            }
          },
          [member, key](toml::table& tbl, const Struct& in) {
            if ((in.*member).has_value()) {
              tbl.insert_or_assign(key, *(in.*member));
            }
          }
      );
    }

    template <typename Struct>
    Field<Struct> optionalBoolField(std::optional<bool> Struct::* member, std::string_view key) {
      return custom<Struct>(
          key,
          [member, key](const toml::table& tbl, Struct& out, std::string_view, Diagnostics&) {
            if (auto v = tbl[key].value<bool>()) {
              out.*member = *v;
            }
          },
          [member, key](toml::table& tbl, const Struct& in) {
            if ((in.*member).has_value()) {
              tbl.insert_or_assign(key, *(in.*member));
            }
          }
      );
    }

    // vector<Enum> <-> array of enum keys; pushes fallback if the parsed list is empty.
    template <typename Struct, typename Enum, std::size_t N>
    Field<Struct> enumArrayField(
        std::vector<Enum> Struct::* member, std::string_view key, const EnumOption<Enum> (&options)[N],
        std::optional<Enum> fallbackIfEmpty
    ) {
      const EnumOption<Enum>* opts = options;
      return custom<Struct>(
          key,
          [member, key, opts, fallbackIfEmpty](const toml::table& tbl, Struct& out, std::string_view, Diagnostics&) {
            const auto* arr = tbl[key].as_array();
            if (arr == nullptr) {
              return;
            }
            (out.*member).clear();
            for (const auto& item : *arr) {
              if (auto s = item.value<std::string>()) {
                if (auto e = enumLookup(opts, N, *s)) {
                  (out.*member).push_back(*e);
                }
              }
            }
            if ((out.*member).empty() && fallbackIfEmpty) {
              (out.*member).push_back(*fallbackIfEmpty);
            }
          },
          [member, key, opts](toml::table& tbl, const Struct& in) {
            toml::array arr;
            for (auto e : in.*member) {
              const std::string_view k = enumKeyOf(opts, N, e);
              if (!k.empty()) {
                arr.push_back(std::string(k));
              }
            }
            tbl.insert_or_assign(key, std::move(arr));
          }
      );
    }

    const Schema<WallpaperAutomationConfig>& wallpaperAutomationSchema() {
      static const Schema<WallpaperAutomationConfig> s = {
          field(&WallpaperAutomationConfig::enabled, "enabled"),
          field(&WallpaperAutomationConfig::intervalSeconds, "interval_seconds", kWallpaperAutomationIntervalRange),
          // order accepts case-insensitive random|alphabetical.
          custom<WallpaperAutomationConfig>(
              "order",
              [](const toml::table& tbl, WallpaperAutomationConfig& out, std::string_view parentPath,
                 Diagnostics& diag) {
                if (auto v = tbl["order"].value<std::string>()) {
                  const std::string lowered = StringUtils::toLower(StringUtils::trim(*v));
                  if (auto parsed = enumFromKey(kWallpaperAutomationOrders, lowered)) {
                    out.order = *parsed;
                  } else {
                    diag.warn(joinPath(parentPath, "order"), "expected random|alphabetical, got \"" + *v + "\"");
                  }
                }
              },
              [](toml::table& tbl, const WallpaperAutomationConfig& in) {
                tbl.insert_or_assign("order", std::string(enumToKey(kWallpaperAutomationOrders, in.order)));
              }
          ),
          field(&WallpaperAutomationConfig::recursive, "recursive"),
      };
      return s;
    }

    const Schema<WallpaperMonitorOverride>& wallpaperMonitorSchema() {
      static const Schema<WallpaperMonitorOverride> s = {
          field(&WallpaperMonitorOverride::match, "match"),
          optionalBoolField(&WallpaperMonitorOverride::enabled, "enabled"),
          colorSpecField(&WallpaperMonitorOverride::fillColor, "fill_color", /*alwaysEmit=*/false),
          optionalPathStringField(&WallpaperMonitorOverride::directory, "directory"),
          optionalPathStringField(&WallpaperMonitorOverride::directoryLight, "directory_light"),
          optionalPathStringField(&WallpaperMonitorOverride::directoryDark, "directory_dark"),
      };
      return s;
    }

    // One keybind action: reads a single chord string or an array of them
    // (warning on an unparseable chord, rethrowing on a hard parse exception);
    // writes the configured chords, or the built-in defaults when none are set.
    Field<KeybindsConfig>
    keybindActionField(std::vector<KeyChord> KeybindsConfig::* member, std::string_view key, KeybindAction action) {
      return custom<KeybindsConfig>(
          key,
          [member, key](const toml::table& tbl, KeybindsConfig& out, std::string_view parentPath, Diagnostics& diag) {
            auto& vec = out.*member;
            vec.clear();
            const auto* node = tbl.get(key);
            if (node == nullptr) {
              return;
            }
            auto parseOne = [&](const std::string& spec) {
              try {
                if (auto chord = parseKeyChordSpec(spec)) {
                  vec.push_back(*chord);
                } else {
                  diag.warn(joinPath(parentPath, key), "invalid keybind chord \"" + spec + "\"");
                }
              } catch (const std::exception& e) {
                throw std::runtime_error(std::format("keybinds.{}: {}", key, e.what()));
              }
            };
            if (auto v = node->value<std::string>()) {
              parseOne(*v);
              return;
            }
            if (const auto* arr = node->as_array()) {
              for (const auto& item : *arr) {
                if (auto v = item.value<std::string>()) {
                  parseOne(*v);
                }
              }
            }
          },
          [member, key, action](toml::table& tbl, const KeybindsConfig& in) {
            const auto& values = in.*member;
            toml::array arr;
            auto emit = [&](const std::vector<KeyChord>& chords) {
              for (const auto& chord : chords) {
                std::string serialized = keyChordToString(chord);
                if (!serialized.empty()) {
                  arr.push_back(std::move(serialized));
                }
              }
            };
            if (values.empty()) {
              emit(defaultKeybindSet(action));
            } else {
              emit(values);
            }
            tbl.insert_or_assign(key, std::move(arr));
          }
      );
    }
  } // namespace

  const Schema<KeybindsConfig>& keybindsSchema() {
    static const Schema<KeybindsConfig> s = {
        keybindActionField(&KeybindsConfig::validate, "validate", KeybindAction::Validate),
        keybindActionField(&KeybindsConfig::cancel, "cancel", KeybindAction::Cancel),
        keybindActionField(&KeybindsConfig::left, "left", KeybindAction::Left),
        keybindActionField(&KeybindsConfig::right, "right", KeybindAction::Right),
        keybindActionField(&KeybindsConfig::up, "up", KeybindAction::Up),
        keybindActionField(&KeybindsConfig::down, "down", KeybindAction::Down),
    };
    return s;
  }

  namespace {
    const Schema<IdleBehaviorConfig>& idleBehaviorSchema() {
      static const Schema<IdleBehaviorConfig> s = {
          field(&IdleBehaviorConfig::enabled, "enabled"),
          field(&IdleBehaviorConfig::timeoutSeconds, "timeout"),
          // action is trimmed on read.
          custom<IdleBehaviorConfig>(
              "action",
              [](const toml::table& tbl, IdleBehaviorConfig& out, std::string_view, Diagnostics&) {
                if (auto v = tbl["action"].value<std::string>()) {
                  out.action = StringUtils::trim(*v);
                }
              },
              [](toml::table& tbl, const IdleBehaviorConfig& in) { tbl.insert_or_assign("action", in.action); }
          ),
          field(&IdleBehaviorConfig::command, "command"),
          field(&IdleBehaviorConfig::resumeCommand, "resume_command"),
          // Emitted only for a bare `suspend` that opts out of pre-suspend locking.
          custom<IdleBehaviorConfig>(
              "lock_before_suspend",
              [](const toml::table& tbl, IdleBehaviorConfig& out, std::string_view, Diagnostics&) {
                if (auto v = tbl["lock_before_suspend"].value<bool>()) {
                  out.lockBeforeSuspend = *v;
                }
              },
              [](toml::table& tbl, const IdleBehaviorConfig& in) {
                if (in.action == "suspend" && !in.lockBeforeSuspend) {
                  tbl.insert_or_assign("lock_before_suspend", false);
                }
              }
          ),
          finalize<IdleBehaviorConfig>([](IdleBehaviorConfig& b, std::string_view, Diagnostics&) {
            normalizeIdleBehaviorAction(b);
          }),
      };
      return s;
    }
  } // namespace

  const Schema<IdleConfig>& idleSchema() {
    static const Schema<IdleConfig> s = {
        field(&IdleConfig::preActionFadeSeconds, "pre_action_fade_seconds", Range<float>{0.0f, 120.0f}),
        // behavior_order is emitted here (vector order); the actual reorder runs
        // last, after the behavior map has been read.
        custom<IdleConfig>(
            "behavior_order", [](const toml::table&, IdleConfig&, std::string_view, Diagnostics&) {},
            [](toml::table& tbl, const IdleConfig& in) {
              toml::array order;
              for (const auto& b : in.behaviors) {
                if (!b.name.empty()) {
                  order.push_back(b.name);
                }
              }
              tbl.insert_or_assign("behavior_order", std::move(order));
            }
        ),
        namedMap<IdleConfig, IdleBehaviorConfig>(
            &IdleConfig::behaviors, "behavior", idleBehaviorSchema(),
            [](IdleBehaviorConfig& b, std::string_view name) { b.name = std::string(name); },
            [](const IdleBehaviorConfig& b) { return b.name; }
        ),
        // Keyless finalizer: reorder behaviors to match behavior_order, leaving
        // any unlisted behaviors in their original relative order at the end.
        custom<IdleConfig>(
            "",
            [](const toml::table& tbl, IdleConfig& out, std::string_view, Diagnostics&) {
              const auto* orderArr = tbl["behavior_order"].as_array();
              if (orderArr == nullptr || out.behaviors.empty()) {
                return;
              }
              std::vector<std::string> orderedNames;
              for (const auto& item : *orderArr) {
                if (auto name = item.value<std::string>(); name && !name->empty()) {
                  orderedNames.push_back(*name);
                }
              }
              if (orderedNames.empty()) {
                return;
              }
              std::unordered_map<std::string, IdleBehaviorConfig> byName;
              for (auto& b : out.behaviors) {
                byName.insert_or_assign(b.name, std::move(b));
              }
              std::vector<IdleBehaviorConfig> ordered;
              ordered.reserve(byName.size());
              for (const auto& name : orderedNames) {
                auto it = byName.find(name);
                if (it == byName.end()) {
                  continue;
                }
                ordered.push_back(std::move(it->second));
                byName.erase(it);
              }
              for (auto& [name, b] : byName) {
                (void)name;
                ordered.push_back(std::move(b));
              }
              out.behaviors = std::move(ordered);
            },
            [](toml::table&, const IdleConfig&) {}
        ),
    };
    return s;
  }

  namespace {
    using TemplateColor = ThemeConfig::TemplateColorConfig;
    using UserTemplate = ThemeConfig::UserTemplateConfig;
    using CompareColor = ThemeConfig::TemplateCompareColorConfig;

    // [theme.templates.custom_colors]: a name-keyed map whose value is either a
    // bare color string or a { color, blend } table. Kept only when name+color
    // are non-empty; emitted only when the list is non-empty.
    Field<ThemeConfig::TemplatesConfig> customColorsField() {
      return custom<ThemeConfig::TemplatesConfig>(
          "custom_colors",
          [](const toml::table& tbl, ThemeConfig::TemplatesConfig& out, std::string_view, Diagnostics&) {
            const auto* map = tbl["custom_colors"].as_table();
            if (map == nullptr) {
              return;
            }
            out.customColors.clear();
            for (const auto& [name, value] : *map) {
              TemplateColor color;
              color.name = std::string(name.str());
              if (const auto* str = value.as_string()) {
                color.color = str->get();
              } else if (const auto* t = value.as_table()) {
                if (auto c = t->get_as<std::string>("color")) {
                  color.color = c->get();
                }
                if (auto b = t->get_as<bool>("blend")) {
                  color.blend = b->get();
                }
              }
              if (!StringUtils::trim(color.name).empty() && !StringUtils::trim(color.color).empty()) {
                out.customColors.push_back(std::move(color));
              }
            }
          },
          [](toml::table& tbl, const ThemeConfig::TemplatesConfig& in) {
            if (in.customColors.empty()) {
              return;
            }
            toml::table map;
            for (const auto& color : in.customColors) {
              toml::table colorTable;
              colorTable.insert_or_assign("color", color.color);
              colorTable.insert_or_assign("blend", color.blend);
              map.insert_or_assign(color.name, std::move(colorTable));
            }
            tbl.insert_or_assign("custom_colors", std::move(map));
          }
      );
    }

    const Schema<UserTemplate>& userTemplateSchema() {
      static const Schema<UserTemplate> s = {
          field(&UserTemplate::enabled, "enabled"),
          field(&UserTemplate::inputPath, "input_path"),
          // input_path_modes is set only when both dark and light are present.
          custom<UserTemplate>(
              "input_path_modes",
              [](const toml::table& tbl, UserTemplate& out, std::string_view, Diagnostics&) {
                const auto* m = tbl["input_path_modes"].as_table();
                if (m == nullptr) {
                  return;
                }
                auto dark = m->get_as<std::string>("dark");
                auto light = m->get_as<std::string>("light");
                if (dark != nullptr && light != nullptr) {
                  out.inputPathModes = ThemeConfig::TemplateInputPathModesConfig{dark->get(), light->get()};
                }
              },
              [](toml::table& tbl, const UserTemplate& in) {
                if (in.inputPathModes.has_value()) {
                  toml::table modes;
                  modes.insert_or_assign("dark", in.inputPathModes->dark);
                  modes.insert_or_assign("light", in.inputPathModes->light);
                  tbl.insert_or_assign("input_path_modes", std::move(modes));
                }
              }
          ),
          // output_path accepts a single string or an array; always emitted as an array.
          custom<UserTemplate>(
              "output_path",
              [](const toml::table& tbl, UserTemplate& out, std::string_view, Diagnostics&) {
                const auto* node = tbl.get("output_path");
                if (node == nullptr) {
                  return;
                }
                out.outputPaths.clear();
                if (const auto* str = node->as_string()) {
                  out.outputPaths.push_back(str->get());
                } else if (const auto* arr = node->as_array()) {
                  for (const auto& item : *arr) {
                    if (const auto* itemStr = item.as_string()) {
                      out.outputPaths.push_back(itemStr->get());
                    }
                  }
                }
              },
              [](toml::table& tbl, const UserTemplate& in) {
                toml::array arr;
                for (const auto& p : in.outputPaths) {
                  arr.push_back(p);
                }
                tbl.insert_or_assign("output_path", std::move(arr));
              }
          ),
          field(&UserTemplate::outputPathDynamic, "output_path_dynamic"),
          field(&UserTemplate::compareTo, "compare_to"),
          // colors_to_compare: array of { name, color }; emitted only when non-empty.
          custom<UserTemplate>(
              "colors_to_compare",
              [](const toml::table& tbl, UserTemplate& out, std::string_view, Diagnostics&) {
                const auto* arr = tbl["colors_to_compare"].as_array();
                if (arr == nullptr) {
                  return;
                }
                out.colorsToCompare.clear();
                for (const auto& item : *arr) {
                  const auto* t = item.as_table();
                  if (t == nullptr) {
                    continue;
                  }
                  auto name = t->get_as<std::string>("name");
                  auto color = t->get_as<std::string>("color");
                  if (name != nullptr && color != nullptr) {
                    out.colorsToCompare.push_back(CompareColor{name->get(), color->get()});
                  }
                }
              },
              [](toml::table& tbl, const UserTemplate& in) {
                if (in.colorsToCompare.empty()) {
                  return;
                }
                toml::array arr;
                for (const auto& color : in.colorsToCompare) {
                  toml::table colorTable;
                  colorTable.insert_or_assign("name", color.name);
                  colorTable.insert_or_assign("color", color.color);
                  arr.push_back(std::move(colorTable));
                }
                tbl.insert_or_assign("colors_to_compare", std::move(arr));
              }
          ),
          field(&UserTemplate::preHook, "pre_hook"),
          field(&UserTemplate::postHook, "post_hook"),
          field(&UserTemplate::index, "index"),
      };
      return s;
    }

    const Schema<ThemeConfig::TemplatesConfig>& templatesSchema() {
      static const Schema<ThemeConfig::TemplatesConfig> s = {
          field(&ThemeConfig::TemplatesConfig::enableBuiltinTemplates, "enable_builtin_templates"),
          field(&ThemeConfig::TemplatesConfig::builtinIds, "builtin_ids"),
          field(&ThemeConfig::TemplatesConfig::enableCommunityTemplates, "enable_community_templates"),
          field(&ThemeConfig::TemplatesConfig::communityIds, "community_ids"),
          customColorsField(),
          namedMap<ThemeConfig::TemplatesConfig, UserTemplate>(
              &ThemeConfig::TemplatesConfig::userTemplates, "user", userTemplateSchema(),
              [](UserTemplate& t, std::string_view name) { t.id = std::string(name); },
              [](const UserTemplate& t) { return t.id; }, /*readSkipEmptyName=*/true
          ),
      };
      return s;
    }
  } // namespace

  const Schema<ThemeConfig>& themeSchema() {
    static const Schema<ThemeConfig> s = {
        enumField(&ThemeConfig::source, "source", kPaletteSources),
        field(&ThemeConfig::builtinPalette, "builtin"),
        field(&ThemeConfig::communityPalette, "community_palette"),
        field(&ThemeConfig::customPalette, "custom_palette"),
        field(&ThemeConfig::wallpaperScheme, "wallpaper_scheme"),
        enumField(&ThemeConfig::mode, "mode", kThemeModes),
        subTable(&ThemeConfig::templates, "templates", templatesSchema()),
    };
    return s;
  }

  namespace {
    // Plain string, but emitted only when non-empty (shell.lang, shell.avatar_path).
    template <typename Struct> Field<Struct> stringIfNonEmptyField(std::string Struct::* member, std::string_view key) {
      return custom<Struct>(
          key,
          [member, key](const toml::table& tbl, Struct& out, std::string_view, Diagnostics&) {
            if (auto v = tbl[key].value<std::string>()) {
              out.*member = *v;
            }
          },
          [member, key](toml::table& tbl, const Struct& in) {
            if (!(in.*member).empty()) {
              tbl.insert_or_assign(key, in.*member);
            }
          }
      );
    }

    const Schema<ShellConfig::AnimationConfig>& shellAnimationSchema() {
      static const Schema<ShellConfig::AnimationConfig> s = {
          field(&ShellConfig::AnimationConfig::enabled, "enabled"),
          field(&ShellConfig::AnimationConfig::speed, "speed", kAnimationSpeedRange),
      };
      return s;
    }

    const Schema<ShellConfig::ShadowConfig>& shellShadowSchema() {
      static const Schema<ShellConfig::ShadowConfig> s = {
          enumField(&ShellConfig::ShadowConfig::direction, "direction", kShadowDirections),
          field(&ShellConfig::ShadowConfig::alpha, "alpha", kUnitRange),
      };
      return s;
    }

    const Schema<ShellConfig::PanelConfig>& shellPanelSchema() {
      static const Schema<ShellConfig::PanelConfig> s = {
          enumField(&ShellConfig::PanelConfig::transparencyMode, "transparency_mode", kPanelTransparencyModes),
          field(&ShellConfig::PanelConfig::borders, "borders"),
          field(&ShellConfig::PanelConfig::shadow, "shadow"),
          enumField(&ShellConfig::PanelConfig::launcherPlacement, "launcher_placement", kPanelPlacements),
          enumField(&ShellConfig::PanelConfig::clipboardPlacement, "clipboard_placement", kPanelPlacements),
          enumField(&ShellConfig::PanelConfig::controlCenterPlacement, "control_center_placement", kPanelPlacements),
          enumField(&ShellConfig::PanelConfig::wallpaperPlacement, "wallpaper_placement", kPanelPlacements),
          enumField(&ShellConfig::PanelConfig::sessionPlacement, "session_placement", kPanelPlacements),
          field(&ShellConfig::PanelConfig::openNearClickControlCenter, "open_near_click_control_center"),
          field(&ShellConfig::PanelConfig::openNearClickLauncher, "open_near_click_launcher"),
          field(&ShellConfig::PanelConfig::openNearClickClipboard, "open_near_click_clipboard"),
          field(&ShellConfig::PanelConfig::openNearClickWallpaper, "open_near_click_wallpaper"),
          field(&ShellConfig::PanelConfig::openNearClickSession, "open_near_click_session"),
          field(&ShellConfig::PanelConfig::launcherCategories, "launcher_categories"),
          field(&ShellConfig::PanelConfig::launcherShowIcons, "launcher_show_icons"),
          field(&ShellConfig::PanelConfig::launcherCompact, "launcher_compact"),
      };
      return s;
    }

    const Schema<ShellConfig::ScreenCornersConfig>& shellScreenCornersSchema() {
      static const Schema<ShellConfig::ScreenCornersConfig> s = {
          field(&ShellConfig::ScreenCornersConfig::enabled, "enabled"),
          field(&ShellConfig::ScreenCornersConfig::size, "size", kScreenCornersSizeRange),
      };
      return s;
    }

    const Schema<ShellConfig::MprisConfig>& shellMprisSchema() {
      static const Schema<ShellConfig::MprisConfig> s = {
          field(&ShellConfig::MprisConfig::blacklist, "blacklist"),
      };
      return s;
    }

    // NOTE: the serializer previously never emitted [shell.screenshot] (read-only gap);
    // including it here fixes the export, mirroring the calendar gap-fix.
    const Schema<ShellConfig::ScreenshotConfig>& shellScreenshotSchema() {
      static const Schema<ShellConfig::ScreenshotConfig> s = {
          field(&ShellConfig::ScreenshotConfig::saveToFile, "save_to_file"),
          field(&ShellConfig::ScreenshotConfig::copyToClipboard, "copy_to_clipboard"),
          field(&ShellConfig::ScreenshotConfig::freezeScreen, "freeze_screen"),
          field(&ShellConfig::ScreenshotConfig::pipeToCommand, "pipe_to_command"),
          field(&ShellConfig::ScreenshotConfig::pipeCommand, "pipe_command"),
          field(&ShellConfig::ScreenshotConfig::directory, "directory"),
          field(&ShellConfig::ScreenshotConfig::filenamePattern, "filename_pattern"),
      };
      return s;
    }

    // command/label/glyph are stored trimmed-or-nullopt but always emitted (value_or("")).
    Field<SessionPanelActionConfig>
    sessionOptionalString(std::optional<std::string> SessionPanelActionConfig::* member, std::string_view key) {
      return custom<SessionPanelActionConfig>(
          key,
          [member, key](const toml::table& tbl, SessionPanelActionConfig& out, std::string_view, Diagnostics&) {
            if (auto v = tbl[key].value<std::string>()) {
              const std::string trimmed = StringUtils::trim(*v);
              out.*member = trimmed.empty() ? std::optional<std::string>{} : std::optional<std::string>{trimmed};
            }
          },
          [member, key](toml::table& tbl, const SessionPanelActionConfig& in) {
            tbl.insert_or_assign(key, (in.*member).value_or(""));
          }
      );
    }

    const Schema<SessionPanelActionConfig>& sessionActionSchema() {
      static const Schema<SessionPanelActionConfig> s = {
          custom<SessionPanelActionConfig>(
              "action",
              [](const toml::table& tbl, SessionPanelActionConfig& out, std::string_view, Diagnostics&) {
                if (auto v = tbl["action"].value<std::string>()) {
                  out.action = StringUtils::toLower(StringUtils::trim(*v));
                }
              },
              [](toml::table& tbl, const SessionPanelActionConfig& in) { tbl.insert_or_assign("action", in.action); }
          ),
          field(&SessionPanelActionConfig::enabled, "enabled"),
          sessionOptionalString(&SessionPanelActionConfig::command, "command"),
          sessionOptionalString(&SessionPanelActionConfig::label, "label"),
          sessionOptionalString(&SessionPanelActionConfig::glyph, "glyph"),
          enumField(&SessionPanelActionConfig::variant, "variant", kSessionActionButtonVariants),
          custom<SessionPanelActionConfig>(
              "shortcut",
              [](const toml::table& tbl, SessionPanelActionConfig& out, std::string_view, Diagnostics&) {
                if (auto v = tbl["shortcut"].value<std::string>()) {
                  const std::string spec = StringUtils::trim(*v);
                  if (!spec.empty()) {
                    out.shortcut = parseKeyChordSpec(spec);
                  }
                }
              },
              [](toml::table& tbl, const SessionPanelActionConfig& in) {
                tbl.insert_or_assign(
                    "shortcut", in.shortcut.has_value() ? keyChordToString(*in.shortcut) : std::string{}
                );
              }
          ),
          // lock_and_suspend never carries a custom command.
          finalize<SessionPanelActionConfig>([](SessionPanelActionConfig& a, std::string_view, Diagnostics&) {
            if (a.action == "lock_and_suspend") {
              a.command = std::nullopt;
            }
          }),
      };
      return s;
    }

    const Schema<ShellSessionConfig>& shellSessionSchema() {
      static const Schema<ShellSessionConfig> s = {
          arrayOf<ShellSessionConfig, SessionPanelActionConfig>(
              &ShellSessionConfig::actions, "actions", sessionActionSchema(),
              [](const SessionPanelActionConfig& a) { return !a.action.empty(); }
          ),
      };
      return s;
    }
  } // namespace

  const Schema<ShellConfig>& shellSchema() {
    static const Schema<ShellConfig> s = {
        field(&ShellConfig::uiScale, "ui_scale", kScaleRange),
        field(&ShellConfig::cornerRadiusScale, "corner_radius_scale", kCornerRadiusScaleRange),
        // font_family is trimmed; empty falls back to sans-serif.
        custom<ShellConfig>(
            "font_family",
            [](const toml::table& tbl, ShellConfig& out, std::string_view, Diagnostics&) {
              if (auto v = tbl["font_family"].value<std::string>()) {
                out.fontFamily = StringUtils::trim(*v);
                if (out.fontFamily.empty()) {
                  out.fontFamily = "sans-serif";
                }
              }
            },
            [](toml::table& tbl, const ShellConfig& in) { tbl.insert_or_assign("font_family", in.fontFamily); }
        ),
        stringIfNonEmptyField(&ShellConfig::lang, "lang"),
        field(&ShellConfig::timeFormat, "time_format"),
        field(&ShellConfig::dateFormat, "date_format"),
        field(&ShellConfig::offlineMode, "offline_mode"),
        field(&ShellConfig::telemetryEnabled, "telemetry_enabled"),
        field(&ShellConfig::setupWizardEnabled, "setup_wizard_enabled"),
        field(&ShellConfig::niriOverviewTypeToLaunchEnabled, "niri_overview_type_to_launch_enabled"),
        field(&ShellConfig::polkitAgent, "polkit_agent"),
        enumField(&ShellConfig::passwordMaskStyle, "password_style", kPasswordMaskStyles),
        field(&ShellConfig::settingsShowAdvanced, "settings_show_advanced"),
        field(&ShellConfig::middleClickOpensWidgetSettings, "middle_click_opens_widget_settings"),
        field(&ShellConfig::showLocation, "show_location"),
        field(&ShellConfig::appIconColorize, "app_icon_colorize"),
        colorSpecField(&ShellConfig::appIconColor, "app_icon_color", /*alwaysEmit=*/false),
        field(&ShellConfig::launchAppsAsSystemdServices, "launch_apps_as_systemd_services"),
        field(&ShellConfig::clipboardEnabled, "clipboard_enabled"),
        field(&ShellConfig::clipboardHistoryMaxEntries, "clipboard_history_max_entries", Range<std::int64_t>{10, 200}),
        field(&ShellConfig::clipboardConfirmClearHistory, "clipboard_confirm_clear_history"),
        field(&ShellConfig::screenTimeEnabled, "screen_time_enabled"),
        field(&ShellConfig::sharedGlContext, "shared_gl_context"),
        field(&ShellConfig::disableMipmaps, "disable_mipmaps"),
        enumField(&ShellConfig::clipboardAutoPaste, "clipboard_auto_paste", kClipboardAutoPasteModes),
        field(&ShellConfig::clipboardImageActionCommand, "clipboard_image_action_command"),
        stringIfNonEmptyField(&ShellConfig::avatarPath, "avatar_path"),
        subTable(&ShellConfig::animation, "animation", shellAnimationSchema()),
        subTable(&ShellConfig::shadow, "shadow", shellShadowSchema()),
        subTable(&ShellConfig::panel, "panel", shellPanelSchema()),
        subTable(&ShellConfig::screenCorners, "screen_corners", shellScreenCornersSchema()),
        subTable(&ShellConfig::mpris, "mpris", shellMprisSchema()),
        subTable(&ShellConfig::screenshot, "screenshot", shellScreenshotSchema()),
        subTable(&ShellConfig::session, "session", shellSessionSchema()),
    };
    return s;
  }

  const Schema<WallpaperConfig>& wallpaperSchema() {
    static const Schema<WallpaperConfig> s = {
        field(&WallpaperConfig::enabled, "enabled"),
        enumField(&WallpaperConfig::fillMode, "fill_mode", kWallpaperFillModes),
        colorSpecField(&WallpaperConfig::fillColor, "fill_color", /*alwaysEmit=*/true),
        enumArrayField(
            &WallpaperConfig::transitions, "transition", kWallpaperTransitions,
            std::optional<WallpaperTransition>{WallpaperTransition::Fade}
        ),
        field(&WallpaperConfig::transitionDurationMs, "transition_duration", kWallpaperTransitionDurationRange),
        field(&WallpaperConfig::edgeSmoothness, "edge_smoothness", kUnitRange),
        field(&WallpaperConfig::transitionOnStartup, "transition_on_startup"),
        pathStringField(&WallpaperConfig::directory, "directory"),
        pathStringField(&WallpaperConfig::directoryLight, "directory_light"),
        pathStringField(&WallpaperConfig::directoryDark, "directory_dark"),
        field(&WallpaperConfig::perMonitorDirectories, "per_monitor_directories"),
        subTable(&WallpaperConfig::automation, "automation", wallpaperAutomationSchema()),
        namedMap<WallpaperConfig, WallpaperMonitorOverride>(
            &WallpaperConfig::monitorOverrides, "monitor", wallpaperMonitorSchema(),
            [](WallpaperMonitorOverride& o, std::string_view name) { o.match = std::string(name); },
            [](const WallpaperMonitorOverride& o) { return o.match; }
        ),
    };
    return s;
  }

  const Schema<HooksConfig>& hooksSchema() {
    // One field per HookKind, keyed by its canonical name. A value may be a single
    // command string or an array; empty entries are dropped (matching the legacy
    // setHookCommandsFromNode). Every kind is always emitted, even when empty.
    static const Schema<HooksConfig> s = [] {
      Schema<HooksConfig> fields;
      for (std::size_t i = 0; i < static_cast<std::size_t>(HookKind::Count); ++i) {
        const std::string_view key = hookKindKey(static_cast<HookKind>(i));
        fields.push_back(
            custom<HooksConfig>(
                key,
                [i, key](const toml::table& tbl, HooksConfig& out, std::string_view, Diagnostics&) {
                  auto& vec = out.commands[i];
                  vec.clear();
                  const auto* node = tbl.get(key);
                  if (node == nullptr) {
                    return;
                  }
                  if (const auto* str = node->as_string()) {
                    if (!str->get().empty()) {
                      vec.push_back(str->get());
                    }
                    return;
                  }
                  if (const auto* arr = node->as_array()) {
                    for (const auto& item : *arr) {
                      if (auto v = item.value<std::string>(); v && !v->empty()) {
                        vec.push_back(*v);
                      }
                    }
                  }
                },
                [i, key](toml::table& tbl, const HooksConfig& in) {
                  toml::array arr;
                  for (const auto& cmd : in.commands[i]) {
                    arr.push_back(cmd);
                  }
                  tbl.insert_or_assign(key, std::move(arr));
                }
            )
        );
      }
      return fields;
    }();
    return s;
  }

  const Schema<CalendarConfig>& calendarSchema() {
    static const Schema<CalendarConfig> s = {
        field(&CalendarConfig::enabled, "enabled"),
        field(&CalendarConfig::refreshMinutes, "refresh_minutes", kRefreshMinutesRange),
        namedMap<CalendarConfig, CalendarConfig::Account>(
            &CalendarConfig::accounts, "account", calendarAccountSchema(),
            [](CalendarConfig::Account& a, std::string_view id) { a.id = std::string(id); },
            [](const CalendarConfig::Account& a) { return a.id; }, true
        ),
    };
    return s;
  }

  namespace {
    // Run collectUnknownKeys for `section`'s schema; false if the section name is
    // unknown. The single dispatch from a section name to its schema.
    bool collectUnknownInSection(std::string_view section, const toml::table& tbl, std::vector<std::string>& unknown) {
      const auto chk = [&](const auto& sch) {
        collectUnknownKeys(tbl, sch, section, unknown);
        return true;
      };
      if (section == "shell") {
        return chk(shellSchema());
      }
      if (section == "wallpaper") {
        return chk(wallpaperSchema());
      }
      if (section == "theme") {
        return chk(themeSchema());
      }
      if (section == "backdrop") {
        return chk(backdropSchema());
      }
      if (section == "lockscreen") {
        return chk(lockscreenSchema());
      }
      if (section == "notification" || section == "notifications") {
        return chk(notificationSchema());
      }
      if (section == "osd") {
        return chk(osdSchema());
      }
      if (section == "system") {
        return chk(systemSchema());
      }
      if (section == "weather") {
        return chk(weatherSchema());
      }
      if (section == "calendar") {
        return chk(calendarSchema());
      }
      if (section == "audio") {
        return chk(audioSchema());
      }
      if (section == "brightness") {
        return chk(brightnessSchema());
      }
      if (section == "battery") {
        return chk(batterySchema());
      }
      if (section == "nightlight") {
        return chk(nightlightSchema());
      }
      if (section == "location") {
        return chk(locationSchema());
      }
      if (section == "idle") {
        return chk(idleSchema());
      }
      if (section == "keybinds") {
        return chk(keybindsSchema());
      }
      if (section == "dock") {
        return chk(dockSchema());
      }
      if (section == "desktop_widgets") {
        return chk(desktopWidgetsSchema());
      }
      if (section == "lockscreen_widgets") {
        return chk(lockscreenWidgetsSchema());
      }
      if (section == "control_center") {
        return chk(controlCenterSchema());
      }
      if (section == "hooks") {
        return chk(hooksSchema());
      }
      return false;
    }

    // Nested table mirroring path[from..] with a dummy leaf, so collectUnknownKeys
    // can report whether the deepest key is recognized.
    toml::table nestedFromPath(const std::vector<std::string>& path, std::size_t from) {
      toml::table root;
      toml::table* cur = &root;
      for (std::size_t i = from; i + 1 < path.size(); ++i) {
        cur->insert_or_assign(path[i], toml::table{});
        cur = cur->get(path[i])->as_table();
      }
      cur->insert_or_assign(path.back(), 0); // dummy leaf
      return root;
    }

    bool isKnownDesktopWidgetPath(const std::vector<std::string>& path) {
      if (path.size() == 2 && path[1] == "widget_order") {
        return true;
      }
      if (path.size() < 2 || path[1] != "widget") {
        return false;
      }
      if (path.size() <= 3) {
        return true;
      }
      static const std::unordered_set<std::string> kWidgetKeys = {
          "id", "type", "output", "cx", "cy", "scale", "rotation", "enabled", "settings",
      };
      if (!kWidgetKeys.contains(path[3])) {
        return false;
      }
      return path.size() == 4 || path[3] == "settings";
    }
  } // namespace

  bool isKnownConfigPath(const std::vector<std::string>& path) {
    if (path.empty()) {
      return false;
    }
    const std::string& section = path[0];

    // Bar lives at the config root (named bars + monitor overrides), not a section
    // schema. {"bar"} / {"bar",name} / {"bar",name,"monitor"[,match]} are container
    // levels; deeper keys validate against the bar field schemas.
    if (section == "bar") {
      if (path.size() <= 2) {
        return true;
      }
      if (path[2] == "monitor") {
        if (path.size() <= 4) {
          return true;
        }
        std::vector<std::string> unknown;
        collectUnknownKeys(nestedFromPath(path, 4), barMonitorOverrideSchema(), "bar", unknown);
        return unknown.empty();
      }
      if (path.size() == 3 && (path[2] == "position" || path[2] == "name")) {
        return true; // emitted/keyed outside barFieldsSchema
      }
      std::vector<std::string> unknown;
      collectUnknownKeys(nestedFromPath(path, 2), barFieldsSchema(), "bar", unknown);
      return unknown.empty();
    }

    if (section == "desktop_widgets" && isKnownDesktopWidgetPath(path)) {
      return true;
    }

    if (path.size() < 2) {
      return false; // a bare section is not a setting path
    }
    std::vector<std::string> unknown;
    if (!collectUnknownInSection(section, nestedFromPath(path, 1), unknown)) {
      return false; // unknown section
    }
    return unknown.empty();
  }

  namespace {
    // Clamp ranges shared by the concrete BarConfig fields and the parallel
    // optional BarMonitorOverride fields — declared once so the two schemas can't
    // drift apart.
    constexpr Range<std::int64_t> kBarThicknessRange{10, 300};
    constexpr Range<std::int64_t> kBarRadiusRange{0, 500};
    constexpr Range<std::int64_t> kBarPanelOverlapRange{-2, 3};
    constexpr Range<float> kBarOpacityRange{0.0f, 1.0f};
    constexpr Range<float> kBarBorderWidthRange{0.0f, 20.0f};
    constexpr Range<float> kBarScaleRange{0.5f, 4.0f};
    constexpr Range<float> kBarCapsulePaddingRange{0.0f, 48.0f};
    constexpr Range<float> kBarCapsuleRadiusRangeF{0.0f, 80.0f};
    constexpr Range<double> kBarCapsulePaddingRangeD{0.0, 48.0};
    constexpr Range<double> kBarCapsuleRadiusRangeD{0.0, 80.0};
    constexpr Range<double> kBarCapsuleOpacityRangeD{0.0, 1.0};

    // Concrete ColorSpec stored as a config string; always emitted. A present
    // non-string value is a hard error (mirrors colorStringValue).
    template <typename Struct> Field<Struct> colorField(ColorSpec Struct::* member, std::string_view key) {
      return custom<Struct>(
          key,
          [member, key](const toml::table& tbl, Struct& out, std::string_view parentPath, Diagnostics&) {
            if (!tbl.contains(key)) {
              return;
            }
            auto v = tbl[key].value<std::string>();
            if (!v) {
              throw std::runtime_error(joinPath(parentPath, key) + ": expected string ColorSpec");
            }
            out.*member = colorSpecFromConfigString(*v, joinPath(parentPath, key));
          },
          [member, key](toml::table& tbl, const Struct& in) {
            tbl.insert_or_assign(key, colorSpecToConfigString(in.*member));
          }
      );
    }

    // optional<ColorSpec>, emitted only when set, read when present. Unlike
    // colorSpecField it does NOT treat an empty string as nullopt — it matches the
    // legacy bar/capsule_group reads (which parse whatever string is present).
    template <typename Struct>
    Field<Struct> optionalColorField(std::optional<ColorSpec> Struct::* member, std::string_view key) {
      return custom<Struct>(
          key,
          [member, key](const toml::table& tbl, Struct& out, std::string_view parentPath, Diagnostics&) {
            if (!tbl.contains(key)) {
              return;
            }
            auto v = tbl[key].value<std::string>();
            if (!v) {
              throw std::runtime_error(joinPath(parentPath, key) + ": expected string ColorSpec");
            }
            out.*member = colorSpecFromConfigString(*v, joinPath(parentPath, key));
          },
          [member, key](toml::table& tbl, const Struct& in) {
            if ((in.*member).has_value()) {
              tbl.insert_or_assign(key, colorSpecToConfigString(*(in.*member)));
            }
          }
      );
    }

    // The capsule_border pair: a bool "specified" flag plus an optional<ColorSpec>.
    // A present key (even empty) sets specified=true; an empty value means "no
    // outline" (nullopt). Emitted only when specified, as the color or empty string.
    template <typename Struct>
    Field<Struct> capsuleBorderField(
        std::optional<ColorSpec> Struct::* colorMember, bool Struct::* specifiedMember, std::string_view key
    ) {
      return custom<Struct>(
          key,
          [colorMember, specifiedMember,
           key](const toml::table& tbl, Struct& out, std::string_view parentPath, Diagnostics&) {
            if (!tbl.contains(key)) {
              return;
            }
            auto v = tbl[key].value<std::string>();
            if (!v) {
              throw std::runtime_error(joinPath(parentPath, key) + ": expected string ColorSpec");
            }
            out.*specifiedMember = true;
            if (StringUtils::trim(*v).empty()) {
              out.*colorMember = std::nullopt;
            } else {
              out.*colorMember = colorSpecFromConfigString(*v, joinPath(parentPath, key));
            }
          },
          [colorMember, specifiedMember, key](toml::table& tbl, const Struct& in) {
            if (in.*specifiedMember) {
              tbl.insert_or_assign(
                  key, (in.*colorMember).has_value() ? colorSpecToConfigString(*(in.*colorMember)) : std::string{}
              );
            }
          }
      );
    }

    template <typename Struct>
    Field<Struct> optionalStringField(std::optional<std::string> Struct::* member, std::string_view key) {
      return custom<Struct>(
          key,
          [member, key](const toml::table& tbl, Struct& out, std::string_view, Diagnostics&) {
            if (auto v = tbl[key].value<std::string>()) {
              out.*member = *v;
            }
          },
          [member, key](toml::table& tbl, const Struct& in) {
            if ((in.*member).has_value()) {
              tbl.insert_or_assign(key, *(in.*member));
            }
          }
      );
    }

    template <typename Struct>
    Field<Struct>
    optionalStringVectorField(std::optional<std::vector<std::string>> Struct::* member, std::string_view key) {
      return custom<Struct>(
          key,
          [member, key](const toml::table& tbl, Struct& out, std::string_view, Diagnostics&) {
            if (auto* arr = tbl[key].as_array()) {
              std::vector<std::string> values;
              for (const auto& item : *arr) {
                if (auto s = item.value<std::string>()) {
                  values.push_back(*s);
                }
              }
              out.*member = std::move(values);
            }
          },
          [member, key](toml::table& tbl, const Struct& in) {
            if ((in.*member).has_value()) {
              toml::array arr;
              for (const auto& v : *(in.*member)) {
                arr.push_back(v);
              }
              tbl.insert_or_assign(key, std::move(arr));
            }
          }
      );
    }

    template <typename Struct>
    Field<Struct> optionalIntField(
        std::optional<std::int32_t> Struct::* member, std::string_view key,
        std::optional<Range<std::int64_t>> range = std::nullopt
    ) {
      return custom<Struct>(
          key,
          [member, key, range](const toml::table& tbl, Struct& out, std::string_view, Diagnostics&) {
            if (auto v = tbl[key].value<std::int64_t>()) {
              const std::int64_t value = range ? applyRange(*v, *range) : *v;
              out.*member = static_cast<std::int32_t>(value);
            }
          },
          [member, key](toml::table& tbl, const Struct& in) {
            if ((in.*member).has_value()) {
              tbl.insert_or_assign(key, static_cast<std::int64_t>(*(in.*member)));
            }
          }
      );
    }

    template <typename Struct>
    Field<Struct> optionalFloatField(
        std::optional<float> Struct::* member, std::string_view key, std::optional<Range<float>> range = std::nullopt
    ) {
      return custom<Struct>(
          key,
          [member, key, range](const toml::table& tbl, Struct& out, std::string_view, Diagnostics&) {
            if (auto v = finiteDouble(tbl[key])) {
              float value = static_cast<float>(*v);
              if (range) {
                value = applyRange(value, *range);
              }
              out.*member = value;
            }
          },
          [member, key](toml::table& tbl, const Struct& in) {
            if ((in.*member).has_value()) {
              tbl.insert_or_assign(key, static_cast<double>(*(in.*member)));
            }
          }
      );
    }

    template <typename Struct>
    Field<Struct> optionalDoubleField(
        std::optional<double> Struct::* member, std::string_view key, std::optional<Range<double>> range = std::nullopt
    ) {
      return custom<Struct>(
          key,
          [member, key, range](const toml::table& tbl, Struct& out, std::string_view, Diagnostics&) {
            if (auto v = finiteDouble(tbl[key])) {
              out.*member = range ? applyRange(*v, *range) : *v;
            }
          },
          [member, key](toml::table& tbl, const Struct& in) {
            if ((in.*member).has_value()) {
              tbl.insert_or_assign(key, *(in.*member));
            }
          }
      );
    }

    const Schema<BarCapsuleGroupStyle>& barCapsuleGroupSchema() {
      static const Schema<BarCapsuleGroupStyle> s = {
          // id is trimmed; empty-id rows are dropped by the consuming keep predicate.
          custom<BarCapsuleGroupStyle>(
              "id",
              [](const toml::table& tbl, BarCapsuleGroupStyle& out, std::string_view, Diagnostics&) {
                if (auto v = tbl["id"].value<std::string>()) {
                  out.id = StringUtils::trim(*v);
                }
              },
              [](toml::table& tbl, const BarCapsuleGroupStyle& in) { tbl.insert_or_assign("id", in.id); }
          ),
          field(&BarCapsuleGroupStyle::members, "members"),
          colorField(&BarCapsuleGroupStyle::fill, "fill"),
          capsuleBorderField(&BarCapsuleGroupStyle::border, &BarCapsuleGroupStyle::borderSpecified, "border"),
          optionalColorField(&BarCapsuleGroupStyle::foreground, "foreground"),
          field(&BarCapsuleGroupStyle::padding, "padding", kBarCapsulePaddingRange),
          optionalFloatField(&BarCapsuleGroupStyle::radius, "radius", kBarCapsuleRadiusRangeF),
          field(&BarCapsuleGroupStyle::opacity, "opacity", kBarOpacityRange),
      };
      return s;
    }

    // layer accepts top|overlay (concrete string member); anything else warns.
    Field<BarConfig> barLayerField() {
      return custom<BarConfig>(
          "layer",
          [](const toml::table& tbl, BarConfig& out, std::string_view parentPath, Diagnostics& diag) {
            if (auto v = tbl["layer"].value<std::string>()) {
              if (*v == "top" || *v == "overlay") {
                out.layer = *v;
              } else {
                diag.warn(joinPath(parentPath, "layer"), "expected top or overlay, got \"" + *v + "\"");
              }
            }
          },
          [](toml::table& tbl, const BarConfig& in) { tbl.insert_or_assign("layer", in.layer); }
      );
    }

    // radius seeds all four corners; per-corner keys below override it.
    Field<BarConfig> barRadiusField() {
      return custom<BarConfig>(
          "radius",
          [](const toml::table& tbl, BarConfig& out, std::string_view, Diagnostics&) {
            if (auto v = tbl["radius"].value<std::int64_t>()) {
              const auto r = static_cast<std::int32_t>(std::clamp<std::int64_t>(*v, 0, 500));
              out.radius = r;
              out.radiusTopLeft = r;
              out.radiusTopRight = r;
              out.radiusBottomLeft = r;
              out.radiusBottomRight = r;
            }
          },
          [](toml::table& tbl, const BarConfig& in) {
            tbl.insert_or_assign("radius", static_cast<std::int64_t>(in.radius));
          }
      );
    }
  } // namespace

  const Schema<BarConfig>& barFieldsSchema() {
    static const Schema<BarConfig> s = {
        field(&BarConfig::enabled, "enabled"),
        field(&BarConfig::autoHide, "auto_hide"),
        field(&BarConfig::reserveSpace, "reserve_space"),
        barLayerField(),
        field(&BarConfig::thickness, "thickness", kBarThicknessRange),
        field(&BarConfig::backgroundOpacity, "background_opacity", kBarOpacityRange),
        colorField(&BarConfig::border, "border"),
        field(&BarConfig::borderWidth, "border_width", kBarBorderWidthRange),
        barRadiusField(),
        field(&BarConfig::radiusTopLeft, "radius_top_left", kBarRadiusRange),
        field(&BarConfig::radiusTopRight, "radius_top_right", kBarRadiusRange),
        field(&BarConfig::radiusBottomLeft, "radius_bottom_left", kBarRadiusRange),
        field(&BarConfig::radiusBottomRight, "radius_bottom_right", kBarRadiusRange),
        field(&BarConfig::marginEnds, "margin_ends"),
        field(&BarConfig::marginEdge, "margin_edge"),
        field(&BarConfig::padding, "padding"),
        field(&BarConfig::widgetSpacing, "widget_spacing"),
        field(&BarConfig::shadow, "shadow"),
        field(&BarConfig::contactShadow, "contact_shadow"),
        field(&BarConfig::panelOverlap, "panel_overlap", kBarPanelOverlapRange),
        field(&BarConfig::scale, "scale", kBarScaleRange),
        field(&BarConfig::fontWeight, "font_weight"),
        field(&BarConfig::startWidgets, "start"),
        field(&BarConfig::centerWidgets, "center"),
        field(&BarConfig::endWidgets, "end"),
        field(&BarConfig::widgetCapsuleDefault, "capsule"),
        colorField(&BarConfig::widgetCapsuleFill, "capsule_fill"),
        optionalColorField(&BarConfig::widgetCapsuleForeground, "capsule_foreground"),
        optionalColorField(&BarConfig::widgetColor, "color"),
        arrayOf<BarConfig, BarCapsuleGroupStyle>(
            &BarConfig::widgetCapsuleGroups, "capsule_group", barCapsuleGroupSchema(),
            [](const BarCapsuleGroupStyle& g) { return !g.id.empty(); }
        ),
        field(&BarConfig::widgetCapsulePadding, "capsule_padding", kBarCapsulePaddingRange),
        optionalDoubleField(&BarConfig::widgetCapsuleRadius, "capsule_radius", kBarCapsuleRadiusRangeD),
        field(&BarConfig::widgetCapsuleOpacity, "capsule_opacity", kBarOpacityRange),
        capsuleBorderField(&BarConfig::widgetCapsuleBorder, &BarConfig::widgetCapsuleBorderSpecified, "capsule_border"),
    };
    return s;
  }

  const Schema<BarMonitorOverride>& barMonitorOverrideSchema() {
    static const Schema<BarMonitorOverride> s = {
        field(&BarMonitorOverride::match, "match"),
        optionalStringField(&BarMonitorOverride::position, "position"),
        optionalBoolField(&BarMonitorOverride::enabled, "enabled"),
        optionalBoolField(&BarMonitorOverride::autoHide, "auto_hide"),
        optionalBoolField(&BarMonitorOverride::reserveSpace, "reserve_space"),
        // layer accepts top|overlay; anything else warns and leaves it unset.
        custom<BarMonitorOverride>(
            "layer",
            [](const toml::table& tbl, BarMonitorOverride& out, std::string_view parentPath, Diagnostics& diag) {
              if (auto v = tbl["layer"].value<std::string>()) {
                if (*v == "top" || *v == "overlay") {
                  out.layer = *v;
                } else {
                  diag.warn(joinPath(parentPath, "layer"), "expected top or overlay, got \"" + *v + "\"");
                }
              }
            },
            [](toml::table& tbl, const BarMonitorOverride& in) {
              if (in.layer.has_value()) {
                tbl.insert_or_assign("layer", *in.layer);
              }
            }
        ),
        optionalIntField(&BarMonitorOverride::thickness, "thickness", kBarThicknessRange),
        optionalFloatField(&BarMonitorOverride::backgroundOpacity, "background_opacity", kBarOpacityRange),
        optionalColorField(&BarMonitorOverride::border, "border"),
        optionalFloatField(&BarMonitorOverride::borderWidth, "border_width", kBarBorderWidthRange),
        optionalIntField(&BarMonitorOverride::radius, "radius", kBarRadiusRange),
        optionalIntField(&BarMonitorOverride::radiusTopLeft, "radius_top_left", kBarRadiusRange),
        optionalIntField(&BarMonitorOverride::radiusTopRight, "radius_top_right", kBarRadiusRange),
        optionalIntField(&BarMonitorOverride::radiusBottomLeft, "radius_bottom_left", kBarRadiusRange),
        optionalIntField(&BarMonitorOverride::radiusBottomRight, "radius_bottom_right", kBarRadiusRange),
        optionalIntField(&BarMonitorOverride::marginEnds, "margin_ends"),
        optionalIntField(&BarMonitorOverride::marginEdge, "margin_edge"),
        optionalIntField(&BarMonitorOverride::padding, "padding"),
        optionalIntField(&BarMonitorOverride::widgetSpacing, "widget_spacing"),
        optionalFloatField(&BarMonitorOverride::scale, "scale", kBarScaleRange),
        optionalBoolField(&BarMonitorOverride::shadow, "shadow"),
        optionalBoolField(&BarMonitorOverride::contactShadow, "contact_shadow"),
        optionalIntField(&BarMonitorOverride::panelOverlap, "panel_overlap", kBarPanelOverlapRange),
        optionalStringVectorField(&BarMonitorOverride::startWidgets, "start"),
        optionalStringVectorField(&BarMonitorOverride::centerWidgets, "center"),
        optionalStringVectorField(&BarMonitorOverride::endWidgets, "end"),
        optionalBoolField(&BarMonitorOverride::widgetCapsuleDefault, "capsule"),
        optionalColorField(&BarMonitorOverride::widgetCapsuleFill, "capsule_fill"),
        optionalColorField(&BarMonitorOverride::widgetCapsuleForeground, "capsule_foreground"),
        optionalColorField(&BarMonitorOverride::widgetColor, "color"),
        optionalDoubleField(&BarMonitorOverride::widgetCapsulePadding, "capsule_padding", kBarCapsulePaddingRangeD),
        optionalDoubleField(&BarMonitorOverride::widgetCapsuleRadius, "capsule_radius", kBarCapsuleRadiusRangeD),
        optionalDoubleField(&BarMonitorOverride::widgetCapsuleOpacity, "capsule_opacity", kBarCapsuleOpacityRangeD),
        capsuleBorderField(
            &BarMonitorOverride::widgetCapsuleBorder, &BarMonitorOverride::widgetCapsuleBorderSpecified,
            "capsule_border"
        ),
        // capsule_group: read-only here (overrides serialize via the resolved bar).
        custom<BarMonitorOverride>(
            "capsule_group",
            [](const toml::table& tbl, BarMonitorOverride& out, std::string_view parentPath, Diagnostics& diag) {
              const auto* arr = tbl["capsule_group"].as_array();
              if (arr == nullptr) {
                return;
              }
              std::vector<BarCapsuleGroupStyle> groups;
              for (const auto& node : *arr) {
                const auto* sub = node.as_table();
                if (sub == nullptr) {
                  continue;
                }
                BarCapsuleGroupStyle g{};
                readInto(*sub, g, barCapsuleGroupSchema(), joinPath(parentPath, "capsule_group"), diag);
                if (!g.id.empty()) {
                  groups.push_back(std::move(g));
                }
              }
              out.widgetCapsuleGroups = std::move(groups);
            },
            [](toml::table&, const BarMonitorOverride&) {}
        ),
    };
    return s;
  }

} // namespace noctalia::config::schema
