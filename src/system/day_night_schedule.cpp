#include "system/day_night_schedule.h"

#include "config/config_types.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <numbers>

namespace day_night_schedule {

  namespace {

    int timeToMinutes(std::string_view hhmm) {
      return (hhmm[0] - '0') * 600 + (hhmm[1] - '0') * 60 + (hhmm[3] - '0') * 10 + (hhmm[4] - '0');
    }

    int currentLocalMinutes() {
      const auto now = std::chrono::system_clock::now();
      const std::time_t t = std::chrono::system_clock::to_time_t(now);
      std::tm local{};
      ::localtime_r(&t, &local);
      return local.tm_hour * 60 + local.tm_min;
    }

    int currentLocalSeconds() {
      const auto now = std::chrono::system_clock::now();
      const std::time_t t = std::chrono::system_clock::to_time_t(now);
      std::tm local{};
      ::localtime_r(&t, &local);
      return local.tm_sec;
    }

    std::chrono::milliseconds sinceBoundaryMs(int nowMin, int nowSec, int lastBoundaryMin) {
      int sinceMin = nowMin - lastBoundaryMin;
      if (sinceMin < 0) {
        sinceMin += 1440;
      }
      return std::chrono::milliseconds(sinceMin * 60 * 1000 + nowSec * 1000);
    }

    bool hasResolvedCoordinates(std::optional<double> latitude, std::optional<double> longitude) {
      return latitude.has_value() && longitude.has_value();
    }

    struct SolarTimes {
      int sunriseMinutes = 0;
      int sunsetMinutes = 0;
    };

    SolarTimes computeSolarTimes(double latitude, double longitude) {
      const auto now = std::chrono::system_clock::now();
      const std::time_t t = std::chrono::system_clock::to_time_t(now);
      std::tm local{};
      ::localtime_r(&t, &local);

      constexpr double kPi = std::numbers::pi;
      const auto dayOfYear = static_cast<double>(local.tm_yday + 1);
      const double fractionalYear = 2.0 * kPi / 365.0 * (dayOfYear - 1.0);

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

      constexpr double kSunriseZenith = 90.833 * kPi / 180.0;
      const double latRad = latitude * kPi / 180.0;
      const double hourAngleArg = std::cos(kSunriseZenith) / (std::cos(latRad) * std::cos(declination))
          - std::tan(latRad) * std::tan(declination);

      if (hourAngleArg > 1.0) {
        return SolarTimes{.sunriseMinutes = 0, .sunsetMinutes = 0};
      }
      if (hourAngleArg < -1.0) {
        return SolarTimes{.sunriseMinutes = 0, .sunsetMinutes = 1440};
      }

      const double hourAngleDeg = std::acos(std::clamp(hourAngleArg, -1.0, 1.0)) * 180.0 / kPi;
      const double timeZoneOffsetMin = static_cast<double>(local.tm_gmtoff) / 60.0;
      const double solarNoonMin = 720.0 - 4.0 * longitude - equationOfTime + timeZoneOffsetMin;

      auto normalizeMinutes = [](double minutes) -> int {
        int rounded = static_cast<int>(std::round(minutes));
        rounded %= 1440;
        if (rounded < 0) {
          rounded += 1440;
        }
        return rounded;
      };

      return SolarTimes{
          .sunriseMinutes = normalizeMinutes(solarNoonMin - hourAngleDeg * 4.0),
          .sunsetMinutes = normalizeMinutes(solarNoonMin + hourAngleDeg * 4.0),
      };
    }

  } // namespace

  std::optional<std::string> normalizedClock(std::string_view value) {
    if (value.size() != 5 || value[2] != ':') {
      return std::nullopt;
    }
    if (!std::isdigit(static_cast<unsigned char>(value[0]))
        || !std::isdigit(static_cast<unsigned char>(value[1]))
        || !std::isdigit(static_cast<unsigned char>(value[3]))
        || !std::isdigit(static_cast<unsigned char>(value[4]))) {
      return std::nullopt;
    }
    const int hour = (value[0] - '0') * 10 + (value[1] - '0');
    const int minute = (value[3] - '0') * 10 + (value[4] - '0');
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
      return std::nullopt;
    }
    return std::string(value);
  }

  GeoCoordinates resolveCoordinates(
      const LocationConfig& config, std::optional<double> resolvedLatitude, std::optional<double> resolvedLongitude
  ) {
    if (hasResolvedCoordinates(resolvedLatitude, resolvedLongitude)) {
      return GeoCoordinates{.latitude = resolvedLatitude, .longitude = resolvedLongitude};
    }
    if (config.latitude.has_value() && config.longitude.has_value()) {
      return GeoCoordinates{.latitude = config.latitude, .longitude = config.longitude};
    }
    return {};
  }

  bool isManualMode(
      const LocationConfig& config, std::optional<double> resolvedLatitude, std::optional<double> resolvedLongitude
  ) {
    // Fixed clock times apply only when no coordinates (resolved or manual) are available.
    return !hasResolvedCoordinates(resolvedLatitude, resolvedLongitude)
        && !(config.latitude.has_value() && config.longitude.has_value())
        && normalizedClock(config.sunset).has_value()
        && normalizedClock(config.sunrise).has_value();
  }

  Evaluation evaluate(
      const LocationConfig& config, std::optional<double> resolvedLatitude, std::optional<double> resolvedLongitude
  ) {
    const int nowMin = currentLocalMinutes();
    const int nowSec = currentLocalSeconds();

    if (isManualMode(config, resolvedLatitude, resolvedLongitude)) {
      const int sunsetMin = timeToMinutes(config.sunset);
      const int sunriseMin = timeToMinutes(config.sunrise);
      const bool night = sunsetMin < sunriseMin ? (nowMin >= sunsetMin && nowMin < sunriseMin)
                                                : (nowMin >= sunsetMin || nowMin < sunriseMin);
      const int targetMin = night ? sunriseMin : sunsetMin;
      int diffMin = targetMin - nowMin;
      if (diffMin <= 0) {
        diffMin += 1440;
      }
      const auto ms = std::chrono::milliseconds(diffMin * 60 * 1000 - nowSec * 1000);
      return Evaluation{
          .night = night,
          .untilBoundary = std::max(ms, std::chrono::milliseconds(1000)),
          .sinceBoundary = sinceBoundaryMs(nowMin, nowSec, night ? sunsetMin : sunriseMin),
      };
    }

    const auto coords = resolveCoordinates(config, resolvedLatitude, resolvedLongitude);
    if (!coords.latitude.has_value() || !coords.longitude.has_value()) {
      return Evaluation{.night = false, .untilBoundary = std::chrono::hours(1)};
    }

    const auto times = computeSolarTimes(*coords.latitude, *coords.longitude);
    if (times.sunriseMinutes == 0 && times.sunsetMinutes == 0) {
      return Evaluation{.night = true, .untilBoundary = std::chrono::hours(1)};
    }
    if (times.sunriseMinutes == 0 && times.sunsetMinutes == 1440) {
      return Evaluation{.night = false, .untilBoundary = std::chrono::hours(1)};
    }

    const int sunset = times.sunsetMinutes;
    const int sunrise = times.sunriseMinutes;
    const bool night =
        sunset > sunrise ? (nowMin >= sunset || nowMin < sunrise) : (nowMin >= sunset && nowMin < sunrise);
    const int targetMin = night ? sunrise : sunset;
    int diffMin = targetMin - nowMin;
    if (diffMin <= 0) {
      diffMin += 1440;
    }
    const auto ms = std::chrono::milliseconds(diffMin * 60 * 1000 - nowSec * 1000);
    return Evaluation{
        .night = night,
        .untilBoundary = std::max(ms, std::chrono::milliseconds(1000)),
        .sinceBoundary = sinceBoundaryMs(nowMin, nowSec, night ? sunset : sunrise),
    };
  }

} // namespace day_night_schedule
