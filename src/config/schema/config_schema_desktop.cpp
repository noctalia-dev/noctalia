#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/config_schema_fields.h"
#include "config/schema/diagnostics.h"
#include "config/schema/engine.h"
#include "config/schema/ranges.h"

#include <string>
#include <string_view>

namespace noctalia::config::schema {

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
        field(&LockscreenConfig::enabled, "enabled"),
        field(&LockscreenConfig::lockBeforeSuspend, "lock_before_suspend"),
        field(&LockscreenConfig::dnd, "dnd"),
        field(&LockscreenConfig::gracePeriodSeconds, "grace_period_seconds", Range<std::int64_t>{0, 60}),
        field(&LockscreenConfig::fingerprint, "fingerprint"),
        field(&LockscreenConfig::allowEmptyPassword, "allow_empty_password"),
        field(&LockscreenConfig::blurredDesktop, "blurred_desktop"),
        enumArrayField(
            &LockscreenConfig::transitions, "transition", kLockscreenTransitions, std::optional<LockscreenTransition>{}
        ),
        field(&LockscreenConfig::transitionDurationMs, "transition_duration", kLockscreenTransitionDurationRange),
        field(&LockscreenConfig::edgeSmoothness, "edge_smoothness", kUnitRange),
        field(&LockscreenConfig::blurIntensity, "blur_intensity", kUnitRange),
        field(&LockscreenConfig::tintIntensity, "tint_intensity", kUnitRange),
        colorField(&LockscreenConfig::tintColor, "tint_color"),
        pathStringField(&LockscreenConfig::wallpaper, "wallpaper"),
        field(&LockscreenConfig::monitors, "monitors"),
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

  const Schema<HotCornersConfig>& hotCornersSchema() {
    static const Schema<HotCornersConfig::Corner> cornerSchema = {
        field(&HotCornersConfig::Corner::action, "action"),
        field(&HotCornersConfig::Corner::command, "command"),
    };
    static const Schema<HotCornersConfig> s = {
        field(&HotCornersConfig::enabled, "enabled"),
        field(&HotCornersConfig::delayMs, "delay_ms", kHotCornersDelayMsRange),
        subTable(&HotCornersConfig::topLeft, "top_left", cornerSchema),
        subTable(&HotCornersConfig::topRight, "top_right", cornerSchema),
        subTable(&HotCornersConfig::bottomLeft, "bottom_left", cornerSchema),
        subTable(&HotCornersConfig::bottomRight, "bottom_right", cornerSchema),
    };
    return s;
  }

  namespace {
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
                } else {
                  warnWrongType(
                      tbl, "order", parentPath, diag,
                      enumKeyList(kWallpaperAutomationOrders, std::size(kWallpaperAutomationOrders))
                  );
                }
              },
              [](toml::table& tbl, const WallpaperAutomationConfig& in) {
                tbl.insert_or_assign("order", std::string(enumToKey(kWallpaperAutomationOrders, in.order)));
              }
          ),
          field(&WallpaperAutomationConfig::recursive, "recursive"),
          field(&WallpaperAutomationConfig::pauseOnFullscreen, "pause_on_fullscreen"),
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
  } // namespace

  const Schema<WallpaperConfig>& wallpaperSchema() {
    static const Schema<WallpaperConfig> s = {
        field(&WallpaperConfig::enabled, "enabled"),
        enumField(&WallpaperConfig::fillMode, "fill_mode", kWallpaperFillModes),
        colorSpecField(&WallpaperConfig::fillColor, "fill_color", /*alwaysEmit=*/true),
        enumArrayField(
            &WallpaperConfig::transitions, "transition", kWallpaperTransitions, std::optional<WallpaperTransition>{}
        ),
        field(&WallpaperConfig::transitionDurationMs, "transition_duration", kWallpaperTransitionDurationRange),
        field(&WallpaperConfig::edgeSmoothness, "edge_smoothness", kUnitRange),
        field(&WallpaperConfig::transitionOnStartup, "transition_on_startup"),
        field(&WallpaperConfig::syncTransitions, "sync_transitions"),
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

} // namespace noctalia::config::schema
