#include "dbus/upower/upower_service.h"
#include "tests/test_check.h"

#include <cmath>
#include <optional>

namespace {

  UPowerDeviceInfo battery(double energyFull, double energyFullDesign) {
    UPowerDeviceInfo info;
    info.energyFull = energyFull;
    info.energyFullDesign = energyFullDesign;
    return info;
  }

  bool nearlyEqual(double lhs, double rhs) { return std::fabs(lhs - rhs) < 1e-9; }

} // namespace

int main() {
  const std::optional<double> worn = battery(50.0, 100.0).healthPercent();
  TEST_CHECK(worn.has_value());
  TEST_CHECK(nearlyEqual(*worn, 50.0));

  // A fuel gauge can learn a full-charge capacity above the vendor's design
  // value, which must not be reported as more than 100% health.
  const std::optional<double> aboveDesign = battery(74.8218, 72.5696).healthPercent();
  TEST_CHECK(aboveDesign.has_value());
  TEST_CHECK(nearlyEqual(*aboveDesign, 100.0));

  TEST_CHECK(!battery(74.8218, 0.0).healthPercent().has_value());
  TEST_CHECK(!battery(0.0, 72.5696).healthPercent().has_value());
  TEST_CHECK(!battery(0.0, 0.0).healthPercent().has_value());

  return 0;
}
