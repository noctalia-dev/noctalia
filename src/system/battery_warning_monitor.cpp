#include "system/battery_warning_monitor.h"

#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "notification/notification_manager.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

  bool isAutoSelector(std::string_view selector) {
    const std::string normalized = StringUtils::toLower(StringUtils::trim(selector));
    return normalized.empty() || normalized == "auto";
  }

  bool isPluggedIn(BatteryState state) {
    return state == BatteryState::Charging
        || state == BatteryState::FullyCharged
        || state == BatteryState::PendingCharge;
  }

  std::string deviceKey(const UPowerDeviceInfo& device) {
    if (!device.path.empty()) {
      return device.path;
    }
    if (!device.nativePath.empty()) {
      return device.nativePath;
    }
    if (!device.serial.empty()) {
      return device.serial;
    }
    return device.model;
  }

  std::string deviceLabel(const UPowerDeviceInfo& device) {
    if (device.isLaptopBattery()) {
      return i18n::tr("notifications.internal.battery");
    }

    const std::string nativeName =
        !device.nativePath.empty() ? StringUtils::pathTail(device.nativePath) : StringUtils::pathTail(device.path);
    if (!device.vendor.empty() && !device.model.empty()) {
      return device.vendor + " " + device.model;
    }
    if (!device.model.empty()) {
      return device.model;
    }
    if (!device.vendor.empty()) {
      return device.vendor;
    }
    if (!nativeName.empty()) {
      return nativeName;
    }
    return i18n::tr("notifications.internal.battery-device");
  }

  bool isSystemBattery(const UPowerDeviceInfo& device, const UPowerDeviceInfo* systemBattery) {
    return systemBattery != nullptr && !device.path.empty() && device.path == systemBattery->path;
  }

} // namespace

int batteryWarningThresholdForDevice(
    const BatteryConfig& config, const UPowerDeviceInfo& device, const UPowerDeviceInfo* systemBattery
) {
  if (!isSystemBattery(device, systemBattery)) {
    for (const auto& deviceThreshold : config.deviceThresholds) {
      if (upowerDeviceMatchesSelector(device, deviceThreshold.selector)) {
        return deviceThreshold.warningThreshold;
      }
    }
  }
  return device.isLaptopBattery() ? config.warningThreshold : 0;
}

int batteryWarningThresholdForSelector(
    const BatteryConfig& config, const UPowerService* upower, std::string_view selector
) {
  if (upower != nullptr) {
    const auto* systemBattery = upower->defaultSystemBattery();
    if (isAutoSelector(selector)) {
      if (systemBattery != nullptr) {
        return batteryWarningThresholdForDevice(config, *systemBattery, systemBattery);
      }
    } else if (const auto* device = upower->deviceForSelector(selector); device != nullptr) {
      return batteryWarningThresholdForDevice(config, *device, systemBattery);
    }
  }
  return config.warningThreshold;
}

namespace {

  // Escalating alert levels (percent), descending. The system battery adds fixed deeper levels at 5%
  // and 2% below the configurable threshold; peripherals warn only at their single configured level.
  std::vector<int> alertPointsForDevice(int threshold, bool isSystem) {
    std::vector<int> points;
    if (threshold <= 0) {
      return points;
    }
    points.push_back(threshold);
    if (isSystem) {
      for (const int deep : {5, 2}) {
        if (deep < threshold) {
          points.push_back(deep);
        }
      }
    }
    return points; // already descending
  }

  // Smallest alert point at or above the current percentage (the most severe level entered), or
  // BatteryWarningMonitor::kNoLevel if the battery is above every alert point.
  int currentLevelFor(const std::vector<int>& points, int percent) {
    int level = BatteryWarningMonitor::kNoLevel;
    for (const int point : points) { // descending → last qualifying assignment is the smallest
      if (point >= percent) {
        level = point;
      }
    }
    return level;
  }

  void fireLowBatteryNotification(
      NotificationManager& notifications, const UPowerDeviceInfo& device, int level, bool isSystem
  ) {
    const int percent = std::clamp(static_cast<int>(std::round(device.state.percentage)), 0, 100);
    const std::string label = deviceLabel(device);

    Urgency urgency = Urgency::Critical;
    int32_t timeout = kDefaultNotificationTimeout * 2;
    std::string title;
    std::string body;
    if (isSystem && level <= 2) {
      timeout = 0; // persistent: imminent shutdown stays on screen until charged or dismissed
      title = i18n::tr("notifications.internal.battery-critical-title");
      body = i18n::tr("notifications.internal.battery-critical-body", "device", label, "percent", percent);
    } else {
      if (isSystem && level > 5) {
        urgency = Urgency::Normal;
        timeout = kDefaultNotificationTimeout;
      }
      title = i18n::tr("notifications.internal.battery-low-title");
      body = i18n::tr("notifications.internal.battery-low-body", "device", label, "percent", percent);
    }

    notifications.addInternal(
        i18n::tr("notifications.internal.battery"), title, body, urgency, timeout,
        std::string("noctalia-glyph:battery-exclamation")
    );
  }

} // namespace

void BatteryWarningMonitor::evaluate(
    const BatteryConfig& config, const UPowerService& upower, NotificationManager& notifications
) {
  std::unordered_set<std::string> seen;
  const auto* systemBattery = upower.defaultSystemBattery();
  for (const auto& device : upower.batteryDevices()) {
    const std::string key = deviceKey(device);
    if (key.empty()) {
      continue;
    }
    seen.insert(key);

    const bool isSystem = isSystemBattery(device, systemBattery);
    const int threshold = batteryWarningThresholdForDevice(config, device, systemBattery);
    auto& state = m_devices[key];

    // Re-arm when charging, absent, or warnings disabled. firedLevel is percentage-based, so a
    // threshold change across config reloads needs no special handling — the next level decides.
    if (!device.state.isPresent || isPluggedIn(device.state.state) || threshold <= 0) {
      state.firedLevel = kNoLevel;
      continue;
    }

    const std::vector<int> points = alertPointsForDevice(threshold, isSystem);
    const int percent = static_cast<int>(std::round(device.state.percentage));
    const int currentLevel = currentLevelFor(points, percent);

    if (currentLevel == kNoLevel) {
      state.firedLevel = kNoLevel; // above every alert point
      continue;
    }

    // Level-triggered: fire once per level as the battery drains into it. Empty state on the first
    // evaluate (startup) fires whatever level the battery already sits in — this is a deliberate
    // safety notification at boot, not a baseline state transition.
    if (currentLevel < state.firedLevel) {
      fireLowBatteryNotification(notifications, device, currentLevel, isSystem);
      state.firedLevel = currentLevel;
    } else if (currentLevel > state.firedLevel) {
      state.firedLevel = currentLevel; // partial recharge → arm so re-dropping re-alerts
    }
  }

  for (auto it = m_devices.begin(); it != m_devices.end();) {
    if (!seen.contains(it->first)) {
      it = m_devices.erase(it);
    } else {
      ++it;
    }
  }
}
