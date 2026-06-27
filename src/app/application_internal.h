#pragma once

// Private helpers shared across the Application translation units
// (application.cpp, application_services.cpp, application_ui.cpp, ...).
// Not part of any public API — include only from src/app/application_*.cpp.

#include "config/config_types.h"
#include "dbus/power/power_profiles_service.h"
#include "i18n/i18n.h"
#include "shell/osd/osd_overlay.h"
#include "system/easyeffects_service.h"
#include "system/gamma_service.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

constexpr bool kLockKeysEnabled = true;

inline bool widgetIsLockKeys(std::string_view widgetName, const Config& config) {
  auto it = config.widgets.find(std::string(widgetName));
  if (it == config.widgets.end()) {
    return widgetName == "lock_keys";
  }
  return it->second.type == "lock_keys";
}

inline bool widgetListHasLockKeys(const std::vector<std::string>& widgets, const Config& config) {
  return std::ranges::any_of(widgets, [&config](const std::string& name) { return widgetIsLockKeys(name, config); });
}

inline bool barMayRender(const BarConfig& bar) {
  if (bar.enabled) {
    return true;
  }
  return std::ranges::any_of(bar.monitorOverrides, [](const BarMonitorOverride& ovr) {
    return ovr.enabled.value_or(false);
  });
}

inline bool configHasLockKeysWidget(const Config& config) {
  return std::ranges::any_of(config.bars, [&config](const BarConfig& bar) {
    return barMayRender(bar)
        && (widgetListHasLockKeys(bar.startWidgets, config)
            || widgetListHasLockKeys(bar.centerWidgets, config)
            || widgetListHasLockKeys(bar.endWidgets, config));
  });
}

inline bool lockKeysConsumersEnabled(const Config& config) {
  return config.osd.kinds.lockKeys || configHasLockKeysWidget(config);
}

inline OsdContent powerProfileOsdContent(std::string_view profile) {
  return OsdContent{
      .kind = OsdKind::PowerProfile,
      .icon = std::string(profileGlyphName(profile)),
      .value = profileLabel(profile),
      .showProgress = false,
  };
}

inline OsdContent effectsProfileOsdContent(AudioEffectsProfileKind kind, std::string_view profile) {
  const char* labelKey = kind == AudioEffectsProfileKind::Input ? "osd.effects.input" : "osd.effects.output";
  return OsdContent{
      .icon = "adjustments",
      .value = i18n::tr(labelKey) + ": " + std::string(profile),
      .showProgress = false,
  };
}

inline OsdContent caffeineOsdContent(bool enabled) {
  return OsdContent{
      .kind = OsdKind::Caffeine,
      .icon = enabled ? "caffeine-on" : "caffeine-off",
      .value = i18n::tr(enabled ? "osd.caffeine.on" : "osd.caffeine.off"),
      .showProgress = false,
      .inactive = !enabled,
  };
}

inline OsdContent nightLightOsdContent(const GammaService& service) {
  std::string icon;
  std::string value;
  const bool inactive = !service.enabled() && !service.forceEnabled();
  if (service.forceEnabled()) {
    icon = "nightlight-forced";
    value = i18n::tr("osd.nightlight.forced");
  } else if (!service.enabled()) {
    icon = "nightlight-off";
    value = i18n::tr("osd.nightlight.off");
  } else if (service.active()) {
    icon = "nightlight-on";
    value = i18n::tr("osd.nightlight.running");
  } else {
    icon = "nightlight-on";
    value = i18n::tr("osd.nightlight.scheduled");
  }
  return OsdContent{
      .kind = OsdKind::NightLight,
      .icon = std::move(icon),
      .value = std::move(value),
      .showProgress = false,
      .inactive = inactive,
  };
}

inline OsdContent dndOsdContent(bool enabled) {
  return OsdContent{
      .kind = OsdKind::Dnd,
      .icon = enabled ? "bell-off" : "bell",
      .value = i18n::tr(enabled ? "osd.dnd.on" : "osd.dnd.off"),
      .showProgress = false,
      .inactive = !enabled,
  };
}

inline OsdContent wifiOsdContent(bool enabled) {
  return OsdContent{
      .kind = OsdKind::Wifi,
      .icon = enabled ? "wifi" : "wifi-off",
      .value = i18n::tr(enabled ? "osd.wifi.on" : "osd.wifi.off"),
      .showProgress = false,
      .inactive = !enabled,
  };
}

inline OsdContent bluetoothOsdContent(bool enabled) {
  return OsdContent{
      .kind = OsdKind::Bluetooth,
      .icon = enabled ? "bluetooth" : "bluetooth-off",
      .value = i18n::tr(enabled ? "osd.bluetooth.on" : "osd.bluetooth.off"),
      .showProgress = false,
      .inactive = !enabled,
  };
}
