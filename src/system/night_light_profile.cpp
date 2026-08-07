#include "system/night_light_profile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <numbers>

namespace night_light_profile {

  namespace {

    struct Waypoint {
      double elevation;
      double warmth;
    };

    // Solar-elevation temperature adjustment is inspired by Redshift. The five waypoints are normalized
    // between 1900K night and 6500K day so Noctalia's configured endpoint temperatures remain authoritative.
    constexpr std::array kWaypoints{
        Waypoint{-18.0, 0.0},       Waypoint{-12.0, 8.0 / 46.0}, Waypoint{-6.0, 15.0 / 46.0},
        Waypoint{0.0, 26.0 / 46.0}, Waypoint{6.0, 1.0},
    };

    constexpr double kDegreesToRadians = std::numbers::pi / 180.0;

  } // namespace

  double solarElevation(std::chrono::system_clock::time_point time, double latitude, double longitude) noexcept {
    if (!std::isfinite(latitude) || !std::isfinite(longitude)) {
      return 0.0;
    }

    latitude = std::clamp(latitude, -90.0, 90.0);
    longitude = std::remainder(longitude, 360.0);

    const std::time_t rawTime = std::chrono::system_clock::to_time_t(time);
    std::tm utc{};
    ::gmtime_r(&rawTime, &utc);

    const double hour = static_cast<double>(utc.tm_hour)
        + static_cast<double>(utc.tm_min) / 60.0
        + static_cast<double>(utc.tm_sec) / 3600.0;
    const double fractionalYear =
        2.0 * std::numbers::pi / 365.0 * (static_cast<double>(utc.tm_yday) + (hour - 12.0) / 24.0);
    const double equationOfTime = 229.18
        * (0.000075
           + 0.001868 * std::cos(fractionalYear)
           - 0.032077 * std::sin(fractionalYear)
           - 0.014615 * std::cos(2.0 * fractionalYear)
           - 0.040849 * std::sin(2.0 * fractionalYear));
    const double declination = 0.006918
        - 0.399912 * std::cos(fractionalYear)
        + 0.070257 * std::sin(fractionalYear)
        - 0.006758 * std::cos(2.0 * fractionalYear)
        + 0.000907 * std::sin(2.0 * fractionalYear)
        - 0.002697 * std::cos(3.0 * fractionalYear)
        + 0.00148 * std::sin(3.0 * fractionalYear);

    double trueSolarMinutes = std::fmod(hour * 60.0 + equationOfTime + 4.0 * longitude, 1440.0);
    if (trueSolarMinutes < 0.0) {
      trueSolarMinutes += 1440.0;
    }
    const double hourAngle = (trueSolarMinutes / 4.0 - 180.0) * kDegreesToRadians;
    const double latitudeRadians = latitude * kDegreesToRadians;
    const double sinElevation = std::sin(latitudeRadians) * std::sin(declination)
        + std::cos(latitudeRadians) * std::cos(declination) * std::cos(hourAngle);
    return std::asin(std::clamp(sinElevation, -1.0, 1.0)) / kDegreesToRadians;
  }

  int temperatureForElevation(double elevation, int dayTemperature, int nightTemperature) noexcept {
    if (elevation <= kWaypoints.front().elevation) {
      return nightTemperature;
    }
    if (elevation >= kWaypoints.back().elevation) {
      return dayTemperature;
    }

    for (std::size_t i = 1; i < kWaypoints.size(); ++i) {
      const Waypoint& lower = kWaypoints[i - 1];
      const Waypoint& upper = kWaypoints[i];
      if (elevation > upper.elevation) {
        continue;
      }
      const double position = (elevation - lower.elevation) / (upper.elevation - lower.elevation);
      const double warmth = std::lerp(lower.warmth, upper.warmth, position);
      return static_cast<int>(
          std::lround(std::lerp(static_cast<double>(nightTemperature), static_cast<double>(dayTemperature), warmth))
      );
    }

    return dayTemperature;
  }

  bool isTransitionElevation(double elevation) noexcept {
    return elevation > kWaypoints.front().elevation && elevation < kWaypoints.back().elevation;
  }

} // namespace night_light_profile
