#pragma once

#include <chrono>
#include <optional>
#include <string_view>

struct LocationConfig;

namespace day_night_schedule {

  struct GeoCoordinates {
    std::optional<double> latitude;
    std::optional<double> longitude;
  };

  struct Evaluation {
    bool night = false;
    std::chrono::milliseconds untilBoundary = std::chrono::hours(1);
    // Time elapsed since the most recent day/night boundary. Used to position a clock-anchored
    // fade ramp so the temperature depends on wall-clock time, not on when the app started.
    std::chrono::milliseconds sinceBoundary = std::chrono::hours(1);
  };

  // resolvedLatitude/resolvedLongitude are the coordinates published by LocationService
  // (IP geolocation or geocoded address). When absent, manual latitude/longitude from the
  // config are used. Fixed sunrise/sunset times are used only when
  // LocationConfig::customSchedule is explicitly true. They are not an automatic
  // fallback when coordinates are unavailable.
  [[nodiscard]] std::optional<std::string> normalizedClock(std::string_view value);
  [[nodiscard]] GeoCoordinates resolveCoordinates(
      const LocationConfig& config, std::optional<double> resolvedLatitude, std::optional<double> resolvedLongitude
  );
  // Both sunset and sunrise parse as HH:MM. Custom scheduling needs this; without it the times
  // cannot drive a schedule and the request is a misconfiguration to surface, not to absorb.
  [[nodiscard]] bool hasUsableCustomTimes(const LocationConfig& config);
  [[nodiscard]] bool isManualMode(const LocationConfig& config);
  [[nodiscard]] Evaluation evaluate(
      const LocationConfig& config, std::optional<double> resolvedLatitude, std::optional<double> resolvedLongitude
  );

} // namespace day_night_schedule
