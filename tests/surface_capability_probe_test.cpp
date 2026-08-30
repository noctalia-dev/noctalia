#include "system/surface_capability_probe.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

  class TempTree {
  public:
    TempTree() {
      const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
      root = std::filesystem::temp_directory_path() / ("noctalia-surface-cap-" + std::to_string(stamp));
      std::filesystem::create_directories(root / "module");
      std::filesystem::create_directories(root / "hwmon");
      // bus/ is created only by addAggregatorBus() so empty trees do not trip the section gate.
    }

    ~TempTree() {
      std::error_code error;
      std::filesystem::remove_all(root, error);
    }

    void addModule(std::string_view name) const {
      std::filesystem::create_directories(root / "module" / name);
    }

    void addAggregatorBus() const {
      std::filesystem::create_directories(root / "bus" / "devices");
    }

    void addHwmon(std::string_view dirName, std::string_view name, std::string_view fileName, std::string_view value)
        const {
      const auto dir = root / "hwmon" / dirName;
      std::filesystem::create_directories(dir);
      {
        std::ofstream out(dir / "name");
        out << name << '\n';
      }
      {
        std::ofstream out(dir / fileName);
        out << value << '\n';
      }
    }

    std::filesystem::path root;
  };

} // namespace

int main() {
  using noctalia::system::surface::probe;

  TempTree empty;
  auto caps = probe(empty.root / "module", empty.root / "bus", empty.root / "hwmon");
  assert(!caps.sectionGate);
  assert(!caps.platformProfileDriver);
  assert(!caps.surfaceBatteryModule);
  assert(!caps.fanRpm);
  assert(!caps.thermalSensors);

  TempTree withAggregator;
  withAggregator.addModule("surface_aggregator");
  caps = probe(withAggregator.root / "module", withAggregator.root / "bus", withAggregator.root / "hwmon");
  assert(caps.sectionGate);
  assert(!caps.platformProfileDriver);

  TempTree withBusOnly;
  withBusOnly.addAggregatorBus();
  caps = probe(withBusOnly.root / "module", withBusOnly.root / "bus", withBusOnly.root / "hwmon");
  assert(caps.sectionGate);

  TempTree full;
  full.addModule("surface_aggregator");
  full.addModule("surface_platform_profile");
  full.addModule("surface_battery");
  full.addModule("surface_hid");
  full.addHwmon("hwmon2", "surface_fan", "fan1_input", "3200");
  full.addHwmon("hwmon4", "surface_thermal", "temp1_input", "45000");
  // Writable slate param under injectable module tree.
  {
    const auto slateDir = full.root / "module" / "surface_aggregator_tabletsw" / "parameters";
    std::filesystem::create_directories(slateDir);
    std::ofstream(slateDir / "tablet_mode_in_slate_state") << "Y\n";
  }
  caps = probe(full.root / "module", full.root / "bus", full.root / "hwmon");
  assert(caps.sectionGate);
  assert(caps.platformProfileDriver);
  assert(caps.surfaceBatteryModule);
  assert(caps.fanRpm);
  assert(caps.thermalSensors);
  assert(caps.typeCoverHotplug);
  assert(caps.slateAsTabletParam);

  TempTree fanWithoutInput;
  fanWithoutInput.addModule("surface_aggregator");
  fanWithoutInput.addHwmon("hwmon2", "surface_fan", "uevent", "DRIVER=surface_fan");
  caps = probe(fanWithoutInput.root / "module", fanWithoutInput.root / "bus", fanWithoutInput.root / "hwmon");
  assert(caps.sectionGate);
  assert(!caps.fanRpm);

  // DMI is not consulted — empty module/bus stays false even if we only have unrelated hwmon.
  TempTree unrelatedHwmon;
  unrelatedHwmon.addHwmon("hwmon0", "coretemp", "temp1_input", "40000");
  caps = probe(unrelatedHwmon.root / "module", unrelatedHwmon.root / "bus", unrelatedHwmon.root / "hwmon");
  assert(!caps.sectionGate);

  return 0;
}
