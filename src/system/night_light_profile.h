#pragma once

#include <chrono>

namespace night_light_profile {

  [[nodiscard]] double
  solarElevation(std::chrono::system_clock::time_point time, double latitude, double longitude) noexcept;

  [[nodiscard]] int temperatureForElevation(double elevation, int dayTemperature, int nightTemperature) noexcept;
  [[nodiscard]] bool isTransitionElevation(double elevation) noexcept;

} // namespace night_light_profile
