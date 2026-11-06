#include "dbus/upower/upower_service.h"
#include "tests/test_check.h"

#include <optional>

namespace {

  UPowerDeviceInfo battery(double energyFull, double energyFullDesign) {
    UPowerDeviceInfo info;
    info.energyFull = energyFull;
    info.energyFullDesign = energyFullDesign;
    return info;
  }

} // namespace

int main() {
  const std::optional<double> worn = battery(50.0, 100.0).healthPercent();
  TEST_NEAR(worn, 50.0, 1e-9);

  // A fuel gauge can learn a full-charge capacity above the vendor's design
  // value, which must not be reported as more than 100% health.
  const std::optional<double> aboveDesign = battery(74.8218, 72.5696).healthPercent();
  TEST_NEAR(aboveDesign, 100.0, 1e-9);

  TEST_CHECK(!battery(74.8218, 0.0).healthPercent().has_value());
  TEST_CHECK(!battery(0.0, 72.5696).healthPercent().has_value());
  TEST_CHECK(!battery(0.0, 0.0).healthPercent().has_value());

  return 0;
}
