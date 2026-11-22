#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/config_schema_fields.h"
#include "config/schema/diagnostics.h"
#include "config/schema/engine.h"
#include "config/schema/ranges.h"
#include "core/input/key_chord.h"

#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace noctalia::config::schema {

  const Schema<AudioConfig>& audioSchema() {
    static const Schema<AudioConfig> s = {
        field(&AudioConfig::colorizeIcons, "colorize_icons"),
        field(&AudioConfig::enableOverdrive, "enable_overdrive"),
        field(&AudioConfig::enableSounds, "enable_sounds"),
        field(&AudioConfig::enableVolumeSounds, "enable_volume_sounds"),
        field(&AudioConfig::enableNotificationSounds, "enable_notification_sounds"),
        field(&AudioConfig::enablePowerSounds, "enable_power_sounds"),
        field(&AudioConfig::enableScreenshotSounds, "enable_screenshot_sounds"),
        field(&AudioConfig::soundVolume, "sound_volume", kUnitRange),
        field(&AudioConfig::soundTheme, "sound_theme"),
        field(&AudioConfig::spectrumFrameRate, "spectrum_frame_rate", kSpectrumFrameRateRange)
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

  namespace {
    // Poll-second floats are stored verbatim here; the [1,120]/disabled clamping
    // happens at consumption, not at parse time; so no Range is attached.
    const Schema<SystemConfig::MonitorConfig>& systemMonitorSchema() {
      static const Schema<SystemConfig::MonitorConfig> s = {
          field(&SystemConfig::MonitorConfig::enabled, "enabled"),
          field(&SystemConfig::MonitorConfig::cpuTempSensorPath, "cpu_temp_sensor_path"),
          field(&SystemConfig::MonitorConfig::cpuPollSeconds, "cpu_poll_seconds"),
          field(&SystemConfig::MonitorConfig::gpuPollSeconds, "gpu_poll_seconds"),
          field(&SystemConfig::MonitorConfig::memoryPollSeconds, "memory_poll_seconds"),
          field(&SystemConfig::MonitorConfig::networkPollSeconds, "network_poll_seconds"),
          field(&SystemConfig::MonitorConfig::diskPollSeconds, "disk_poll_seconds"),
          field(&SystemConfig::MonitorConfig::cpuUsageActivityThreshold, "cpu_usage_activity_threshold"),
          field(&SystemConfig::MonitorConfig::cpuUsageCriticalThreshold, "cpu_usage_critical_threshold"),
          field(&SystemConfig::MonitorConfig::cpuTempActivityThreshold, "cpu_temp_activity_threshold"),
          field(&SystemConfig::MonitorConfig::cpuTempCriticalThreshold, "cpu_temp_critical_threshold"),
          field(&SystemConfig::MonitorConfig::cpuFreqActivityThreshold, "cpu_freq_activity_threshold"),
          field(&SystemConfig::MonitorConfig::cpuFreqCriticalThreshold, "cpu_freq_critical_threshold"),
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
          field(&SystemConfig::MonitorConfig::diskUsedPctActivityThreshold, "disk_used_pct_activity_threshold"),
          field(&SystemConfig::MonitorConfig::diskUsedPctCriticalThreshold, "disk_used_pct_critical_threshold"),
          field(&SystemConfig::MonitorConfig::diskUsedActivityThreshold, "disk_used_activity_threshold"),
          field(&SystemConfig::MonitorConfig::diskUsedCriticalThreshold, "disk_used_critical_threshold"),
          field(&SystemConfig::MonitorConfig::diskFreePctActivityThreshold, "disk_free_pct_activity_threshold"),
          field(&SystemConfig::MonitorConfig::diskFreePctCriticalThreshold, "disk_free_pct_critical_threshold"),
          field(&SystemConfig::MonitorConfig::diskFreeActivityThreshold, "disk_free_activity_threshold"),
          field(&SystemConfig::MonitorConfig::diskFreeCriticalThreshold, "disk_free_critical_threshold"),
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
        field(&LocationConfig::autoLocate, "auto_locate"),
        field(&LocationConfig::address, "address"),
        field(&LocationConfig::customSchedule, "custom_schedule"),
        field(&LocationConfig::sunset, "sunset"),
        field(&LocationConfig::sunrise, "sunrise"),
        field(&LocationConfig::latitude, "latitude"),
        field(&LocationConfig::longitude, "longitude"),
    };
    return s;
  }

  namespace {
    const Schema<BrightnessMonitorOverride>& brightnessMonitorSchema() {
      static const Schema<BrightnessMonitorOverride> s = {
          field(&BrightnessMonitorOverride::match, "match"),
          optionalEnumField(&BrightnessMonitorOverride::backend, "backend", kBrightnessBackendPreferences),
          field(&BrightnessMonitorOverride::backlightDevice, "backlight_device"),
          field(&BrightnessMonitorOverride::ddcBus, "ddc_bus"),
          optionalFloatField(
              &BrightnessMonitorOverride::scheduledDayBrightness, "scheduled_day_brightness", kUnitRange
          ),
          optionalFloatField(
              &BrightnessMonitorOverride::scheduledNightBrightness, "scheduled_night_brightness", kUnitRange
          ),
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
        field(&BrightnessConfig::syncBrightnessOfAllMonitors, "sync_all_monitors"),
        field(&BrightnessConfig::ddcutilIgnoreMmids, "ignore_mmids"),
        field(&BrightnessConfig::minimumBrightness, "minimum_brightness", kUnitRange),

        field(&BrightnessConfig::enableScheduledBrightness, "enable_scheduled_brightness"),
        field(&BrightnessConfig::scheduledNightBrightness, "scheduled_night_brightness", kUnitRange),
        field(&BrightnessConfig::scheduledDayBrightness, "scheduled_day_brightness", kUnitRange),

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

  const Schema<BluetoothConfig>& bluetoothSchema() {
    static const Schema<BluetoothConfig> s = {
        field(&BluetoothConfig::connectWhenEnabled, "connect_when_enabled"),
        field(&BluetoothConfig::connectionNotifications, "connection_notifications"),
    };
    return s;
  }

  namespace {
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
              for (const auto& spec : readStringArray(*arr, joinPath(parentPath, key), diag)) {
                parseOne(spec);
              }
            } else {
              warnWrongType(diag, joinPath(parentPath, key), *node, "a string or an array of strings");
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
        keybindActionField(&KeybindsConfig::tabNext, "tab_next", KeybindAction::TabNext),
        keybindActionField(&KeybindsConfig::tabPrevious, "tab_previous", KeybindAction::TabPrevious),
        keybindActionField(&KeybindsConfig::categoryNext, "category_next", KeybindAction::CategoryNext),
        keybindActionField(&KeybindsConfig::categoryPrevious, "category_previous", KeybindAction::CategoryPrevious),
        keybindActionField(&KeybindsConfig::deleteEntry, "delete", KeybindAction::Delete),
        keybindActionField(&KeybindsConfig::copy, "copy", KeybindAction::Copy),
        keybindActionField(&KeybindsConfig::save, "save", KeybindAction::Save),
        keybindActionField(&KeybindsConfig::selectAll, "select_all", KeybindAction::SelectAll),
    };
    return s;
  }

  namespace {
    const Schema<IdleBehaviorConfig>& idleBehaviorSchema() {
      static const Schema<IdleBehaviorConfig> s = {
          field(&IdleBehaviorConfig::enabled, "enabled"),
          field(&IdleBehaviorConfig::timeoutSeconds, "timeout"),
          field(&IdleBehaviorConfig::lockedTimeoutSeconds, "locked_timeout"),
          // action is trimmed on read.
          custom<IdleBehaviorConfig>(
              "action",
              [](const toml::table& tbl, IdleBehaviorConfig& out, std::string_view parentPath, Diagnostics& diag) {
                if (auto v = tbl["action"].value<std::string>()) {
                  out.action = StringUtils::trim(*v);
                } else {
                  warnWrongType(tbl, "action", parentPath, diag, "a string");
                }
              },
              [](toml::table& tbl, const IdleBehaviorConfig& in) { tbl.insert_or_assign("action", in.action); }
          ),
          field(&IdleBehaviorConfig::command, "command"),
          field(&IdleBehaviorConfig::resumeCommand, "resume_command"),
          // Emitted only for a bare `suspend` that opts out of pre-suspend locking.
          custom<IdleBehaviorConfig>(
              "lock_before_suspend",
              [](const toml::table& tbl, IdleBehaviorConfig& out, std::string_view parentPath, Diagnostics& diag) {
                if (auto v = tbl["lock_before_suspend"].value<bool>()) {
                  out.lockBeforeSuspend = *v;
                } else {
                  warnWrongType(tbl, "lock_before_suspend", parentPath, diag, "a boolean");
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
        field(&IdleConfig::preActionFadeSeconds, "pre_action_fade_seconds", Range<float>{0.0F, 120.0F}),
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
            [](const toml::table& tbl, IdleConfig& out, std::string_view parentPath, Diagnostics& diag) {
              const auto* orderArr = tbl["behavior_order"].as_array();
              if (orderArr == nullptr) {
                warnWrongType(tbl, "behavior_order", parentPath, diag, "an array of strings");
              }
              if (orderArr == nullptr || out.behaviors.empty()) {
                return;
              }
              std::vector<std::string> orderedNames;
              for (auto& name : readStringArray(*orderArr, joinPath(parentPath, "behavior_order"), diag)) {
                if (!name.empty()) {
                  orderedNames.push_back(std::move(name));
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
                [i, key](const toml::table& tbl, HooksConfig& out, std::string_view parentPath, Diagnostics& diag) {
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
                    for (auto& v : readStringArray(*arr, joinPath(parentPath, key), diag)) {
                      if (!v.empty()) {
                        vec.push_back(std::move(v));
                      }
                    }
                  } else {
                    warnWrongType(diag, joinPath(parentPath, key), *node, "a string or an array of strings");
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

  const Schema<StorageConfig>& storageSchema() {
    static const Schema<StorageConfig> s = {
        custom<StorageConfig>(
            "key_source",
            [](const toml::table& table, StorageConfig& out, std::string_view parentPath, Diagnostics& diag) {
              const auto value = table["key_source"].value<std::string>();
              if (!value.has_value()) {
                warnWrongType(table, "key_source", parentPath, diag, "a string");
                return;
              }
              const std::string source = StringUtils::trim(*value);
              if (source == "secret-service") {
                out.keySource = StorageKeySource::SecretService;
              } else if (source == "file") {
                out.keySource = StorageKeySource::File;
              } else {
                diag.error(joinPath(parentPath, "key_source"), R"(key_source must be "secret-service" or "file")");
              }
            },
            [](toml::table& table, const StorageConfig& in) {
              table.insert_or_assign("key_source", in.keySource == StorageKeySource::File ? "file" : "secret-service");
            }
        ),
        pathStringField(&StorageConfig::keyFile, "key_file"),
        finalize<StorageConfig>([](StorageConfig& out, std::string_view parentPath, Diagnostics& diag) {
          if (out.keySource == StorageKeySource::File) {
            if (out.keyFile.empty()) {
              diag.error(joinPath(parentPath, "key_file"), R"(storage with key_source = "file" requires key_file)");
            } else if (!std::filesystem::path(out.keyFile).is_absolute()) {
              diag.error(joinPath(parentPath, "key_file"), "key_file must resolve to an absolute path");
            }
          } else if (!out.keyFile.empty()) {
            diag.error(joinPath(parentPath, "key_file"), R"(key_file requires key_source = "file")");
          }
        }),
    };
    return s;
  }

} // namespace noctalia::config::schema
