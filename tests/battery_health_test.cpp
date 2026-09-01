#include "dbus/upower/upower_service.h"
#include "tests/test_check.h"

#include <cmath>

namespace {

  UPowerDeviceInfo battery(double energyFull, double energyFullDesign) {
    UPowerDeviceInfo info;
    info.type = UPowerDeviceType::Battery;
    info.powerSupply = true;
    info.isPresent = true;
    info.energyFull = energyFull;
    info.energyFullDesign = energyFullDesign;
    return info;
  }

  bool nearlyEqual(double lhs, double rhs) { return std::fabs(lhs - rhs) < 1e-9; }

} // namespace

int main() {
  const UPowerDeviceInfo worn = battery(50.0, 100.0);
  TEST_CHECK(worn.hasHealth());
  TEST_CHECK(nearlyEqual(worn.healthPercent(), 50.0));

  // A fuel gauge can learn a full-charge capacity above the vendor's design
  // value, which must not be reported as more than 100% health.
  const UPowerDeviceInfo aboveDesign = battery(74.8218, 72.5696);
  TEST_CHECK(aboveDesign.hasHealth());
  TEST_CHECK(nearlyEqual(aboveDesign.healthPercent(), 100.0));

  TEST_CHECK(!battery(74.8218, 0.0).hasHealth());
  TEST_CHECK(!battery(0.0, 72.5696).hasHealth());
  TEST_CHECK(nearlyEqual(battery(74.8218, 0.0).healthPercent(), 0.0));

  return 0;
}
