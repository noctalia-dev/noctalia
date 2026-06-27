#pragma once

#include "config/config_types.h"

#include <string>
#include <string_view>
#include <unordered_map>

class NotificationManager;
class UPowerService;
struct UPowerDeviceInfo;

[[nodiscard]] int batteryWarningThresholdForDevice(
    const BatteryConfig& config, const UPowerDeviceInfo& device, const UPowerDeviceInfo* systemBattery = nullptr
);
[[nodiscard]] int
batteryWarningThresholdForSelector(const BatteryConfig& config, const UPowerService* upower, std::string_view selector);

class BatteryWarningMonitor {
public:
  // firedLevel is the percentage of the deepest alert level already notified this discharge cycle;
  // kNoLevel means none fired yet (so the next evaluate fires whatever level the battery is in).
  static constexpr int kNoLevel = 101;

  // Level-triggered: re-evaluates live battery levels and fires escalating low-battery
  // notifications. Safe to call at startup, on UPower changes, and on config reload — the
  // per-device fired-level bookkeeping keeps each level to a single notification per discharge cycle.
  void evaluate(const BatteryConfig& config, const UPowerService& upower, NotificationManager& notifications);

private:
  struct DeviceWarningState {
    int firedLevel = kNoLevel;
  };

  std::unordered_map<std::string, DeviceWarningState> m_devices;
};
