#include "dbus/upower/upower_service.h"
#include "tests/test_check.h"

namespace {

  UPowerDeviceInfo device() {
    UPowerDeviceInfo info;
    info.path = "/org/freedesktop/UPower/devices/battery_BAT0";
    info.nativePath = "BAT0";
    info.vendor = "Noctalia";
    info.model = "Test Battery";
    info.serial = "battery-1";
    info.type = UPowerDeviceType::Battery;
    info.powerSupply = true;
    info.isPresent = true;
    info.state.isPresent = true;
    return info;
  }

  template <typename Mutator> void expectCatalogChange(Mutator mutate) {
    const UPowerDeviceInfo original = device();
    UPowerDeviceInfo changed = original;
    mutate(changed);
    TEST_CHECK(!original.sameCatalogEntry(changed));
  }

} // namespace

int main() {
  const UPowerDeviceInfo original = device();
  UPowerDeviceInfo telemetry = original;
  telemetry.energyFull = 48.0;
  telemetry.energyFullDesign = 52.0;
  telemetry.state.percentage = 42.0;
  telemetry.state.energyRate = 18.5;
  telemetry.state.state = BatteryState::Charging;
  telemetry.state.timeToEmpty = 1200;
  telemetry.state.timeToFull = 2400;
  telemetry.state.energy = 32.0;
  telemetry.state.onBattery = true;
  TEST_CHECK(original.sameCatalogEntry(telemetry));

  expectCatalogChange([](UPowerDeviceInfo& info) { info.path += "-new"; });
  expectCatalogChange([](UPowerDeviceInfo& info) { info.nativePath = "BAT1"; });
  expectCatalogChange([](UPowerDeviceInfo& info) { info.vendor = "Other"; });
  expectCatalogChange([](UPowerDeviceInfo& info) { info.model = "Other Battery"; });
  expectCatalogChange([](UPowerDeviceInfo& info) { info.serial = "battery-2"; });
  expectCatalogChange([](UPowerDeviceInfo& info) { info.type = UPowerDeviceType::Ups; });
  expectCatalogChange([](UPowerDeviceInfo& info) { info.powerSupply = false; });
  expectCatalogChange([](UPowerDeviceInfo& info) { info.isPresent = false; });

  return 0;
}
