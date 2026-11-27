#include "system/night_light_temperature_override.h"
#include "tests/test_check.h"

#include <cstdint>
#include <optional>

namespace {

  constexpr std::optional<std::uint64_t> kCli = std::nullopt;

  void testOnlyTheOwningPluginReleases() {
    NightLightTemperatureOverride temperature;
    TEST_CHECK(temperature.set(4500, 1));
    TEST_CHECK(temperature.set(5000, 2));
    TEST_CHECK(!temperature.release(1));
    TEST_CHECK(temperature.kelvin() == 5000);
    TEST_CHECK(temperature.release(2));
    TEST_CHECK(!temperature.kelvin().has_value());
    TEST_CHECK(!temperature.release(2));
  }

  void testPluginSameValueKeepsCliOwnership() {
    // The CLI sets 4500, then a plugin on a timer sets the same value: the plugin stopping must not
    // clear the temperature the CLI set.
    NightLightTemperatureOverride temperature;
    TEST_CHECK(temperature.set(4500, kCli));
    TEST_CHECK(!temperature.set(4500, 1));
    TEST_CHECK(!temperature.release(1));
    TEST_CHECK(temperature.kelvin() == 4500);
  }

  void testCliSameValueTakesOverFromPlugin() {
    // A plugin owns 4500 and the CLI sets 4500 again: the CLI now owns it.
    NightLightTemperatureOverride temperature;
    TEST_CHECK(temperature.set(4500, 1));
    TEST_CHECK(!temperature.set(4500, kCli));
    TEST_CHECK(!temperature.release(1));
    TEST_CHECK(temperature.kelvin() == 4500);
  }

  void testPluginSameValueKeepsPluginOwnership() {
    NightLightTemperatureOverride temperature;
    TEST_CHECK(temperature.set(4500, 1));
    TEST_CHECK(!temperature.set(4500, 2));
    TEST_CHECK(!temperature.release(2));
    TEST_CHECK(temperature.release(1));
  }

  void testReplacementByAnotherPluginSurvivesTeardown() {
    // A reloaded runtime gets a new host id; once it sets a different temperature, the old runtime's
    // deferred release leaves it alone.
    NightLightTemperatureOverride temperature;
    TEST_CHECK(temperature.set(4500, 1));
    TEST_CHECK(temperature.set(4600, 2));
    TEST_CHECK(!temperature.release(1));
    TEST_CHECK(temperature.kelvin() == 4600);
  }

  void testClearAndResetDropOwnership() {
    NightLightTemperatureOverride temperature;
    TEST_CHECK(temperature.set(4500, 1));
    TEST_CHECK(temperature.set(std::nullopt, kCli));
    TEST_CHECK(!temperature.set(std::nullopt, 1));
    TEST_CHECK(!temperature.release(1));

    TEST_CHECK(temperature.set(4500, 1));
    TEST_CHECK(temperature.reset());
    TEST_CHECK(!temperature.reset());
    TEST_CHECK(!temperature.release(1));

    // A plugin setting a value after a reset owns it afresh.
    TEST_CHECK(temperature.set(4500, 1));
    TEST_CHECK(temperature.release(1));
  }

} // namespace

int main() {
  testOnlyTheOwningPluginReleases();
  testPluginSameValueKeepsCliOwnership();
  testCliSameValueTakesOverFromPlugin();
  testPluginSameValueKeepsPluginOwnership();
  testReplacementByAnotherPluginSurvivesTeardown();
  testClearAndResetDropOwnership();
  return 0;
}
