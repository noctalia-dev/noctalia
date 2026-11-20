#pragma once

#include "shell/render/shell_render_styles.h"

#include <cstdint>
#include <optional>

// Maps a current weather code and day/night state to the effect rendered behind a weather card
// (the Weather tab's current-conditions card, the Home tab's Date/Time+Weather card, ...).
// Shared so every location that shows the weather-effect background derives it the same way.
WeatherEffectType effectForWeatherCode(std::int32_t code, bool isDay);

// NOCTALIA_WEATHER_EFFECT=sun|snow|rain|cloud|fog|stars forces that effect regardless of the
// current weather and the effects setting, for testing the shaders. Cached after first read.
std::optional<WeatherEffectType> weatherEffectOverride();
