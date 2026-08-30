#pragma once

#include <filesystem>

namespace noctalia::system::surface {

  /// Capability snapshot for linux-surface hosts. Roots are injectable for tests.
  struct Capabilities {
    /// True when Surface Aggregator Module infrastructure is present (not DMI alone).
    bool sectionGate = false;
    bool platformProfileDriver = false;
    bool surfaceBatteryModule = false;
    bool fanRpm = false;
    bool thermalSensors = false;
    bool tabletModeSwitch = false;
    bool alsSensor = false;
    bool accelSensor = false;
    /// Module param `tablet_mode_in_slate_state` is present and writable by this process.
    bool slateAsTabletParam = false;
    /// Type Cover hotplug can be inferred (`surface_hid` / `surface_hotplug`).
    bool typeCoverHotplug = false;

    bool operator==(const Capabilities&) const = default;
  };

  /// Probe Surface kernel capabilities.
  /// Defaults match a live linux-surface host; tests pass temporary trees.
  [[nodiscard]] Capabilities probe(
      const std::filesystem::path& moduleRoot = "/sys/module",
      const std::filesystem::path& aggregatorBusRoot = "/sys/bus/surface_aggregator",
      const std::filesystem::path& hwmonRoot = "/sys/class/hwmon",
      const std::filesystem::path& iioRoot = "/sys/bus/iio/devices",
      const std::filesystem::path& inputClassRoot = "/sys/class/input"
  );

} // namespace noctalia::system::surface
