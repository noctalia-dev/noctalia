#include "system/night_light_profile.h"

#include <chrono>
#include <cstdio>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::fprintf(stderr, "night_light_profile_test: %s\n", message);
      return false;
    }
    return true;
  }

} // namespace

int main() {
  using namespace std::chrono;

  bool ok = true;
  constexpr int kDay = 6500;
  constexpr int kNight = 1900;

  ok = expect(
           night_light_profile::temperatureForElevation(-30.0, kDay, kNight) == 1900,
           "elevations below astronomical twilight use the night endpoint"
       )
      && ok;
  ok = expect(
           night_light_profile::temperatureForElevation(-12.0, kDay, kNight) == 2700,
           "nautical twilight uses the configured profile temperature"
       )
      && ok;
  ok = expect(
           night_light_profile::temperatureForElevation(-6.0, kDay, kNight) == 3400,
           "civil twilight uses the configured profile temperature"
       )
      && ok;
  ok = expect(
           night_light_profile::temperatureForElevation(0.0, kDay, kNight) == 4500,
           "the horizon uses the configured profile temperature"
       )
      && ok;
  ok = expect(night_light_profile::temperatureForElevation(6.0, kDay, kNight) == 6500, "daylight uses the day endpoint")
      && ok;
  ok = expect(
           night_light_profile::temperatureForElevation(-3.0, kDay, kNight) == 3950,
           "temperatures interpolate continuously between twilight phases"
       )
      && ok;
  ok = expect(
           night_light_profile::temperatureForElevation(0.0, 6500, 4000) == 5413,
           "the profile scales to configured Noctalia endpoints"
       )
      && ok;

  ok = expect(!night_light_profile::isTransitionElevation(-18.0), "deep night is not transitioning") && ok;
  ok = expect(night_light_profile::isTransitionElevation(0.0), "twilight is transitioning") && ok;
  ok = expect(!night_light_profile::isTransitionElevation(6.0), "full daylight is not transitioning") && ok;

  const auto equinoxNoon = sys_days{year{2026} / March / day{20}} + hours{12};
  const auto equinoxMidnight = sys_days{year{2026} / March / day{20}};
  ok = expect(
           night_light_profile::solarElevation(equinoxNoon, 0.0, 0.0) > 87.0,
           "equinox noon is near the zenith at the equator"
       )
      && ok;
  ok = expect(
           night_light_profile::solarElevation(equinoxMidnight, 0.0, 0.0) < -87.0,
           "equinox midnight is well below the horizon at the equator"
       )
      && ok;

  return ok ? 0 : 1;
}
