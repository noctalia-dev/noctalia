#include "shell/render/weather_effect_mapping.h"

#include "core/log.h"

#include <array>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace {
  constexpr Logger kLog("weather_effect_mapping");
}

WeatherEffectType effectForWeatherCode(std::int32_t code, bool isDay) {
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    return WeatherEffectType::Rain;
  }
  if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) {
    return WeatherEffectType::Snow;
  }
  if (code == 3) {
    return WeatherEffectType::Cloud;
  }
  if (code >= 40 && code <= 49) {
    return WeatherEffectType::Fog;
  }
  if (code <= 2 && isDay) {
    return WeatherEffectType::Sun;
  }
  if (code <= 2 && !isDay) {
    return WeatherEffectType::Stars;
  }
  // Interim stand-in: thunderstorms have no dedicated lightning effect yet, and
  // a flat None card is strictly worse than the closest visual approximation.
  if (code >= 95 && code <= 99) {
    return WeatherEffectType::Rain;
  }
  return WeatherEffectType::None;
}

std::optional<WeatherEffectType> weatherEffectOverride() {
  static const std::optional<WeatherEffectType> value = [] -> std::optional<WeatherEffectType> {
    const char* env = std::getenv("NOCTALIA_WEATHER_EFFECT");
    if (env == nullptr || env[0] == '\0') {
      return std::nullopt;
    }
    static constexpr std::array<std::pair<std::string_view, WeatherEffectType>, 6> kNames = {{
        {"sun", WeatherEffectType::Sun},
        {"snow", WeatherEffectType::Snow},
        {"rain", WeatherEffectType::Rain},
        {"cloud", WeatherEffectType::Cloud},
        {"fog", WeatherEffectType::Fog},
        {"stars", WeatherEffectType::Stars},
    }};
    const std::string_view name(env);
    for (const auto& [key, type] : kNames) {
      if (key == name) {
        return type;
      }
    }
    kLog.warn("ignoring NOCTALIA_WEATHER_EFFECT={}: expected sun, snow, rain, cloud, fog, or stars", name);
    return std::nullopt;
  }();
  return value;
}
