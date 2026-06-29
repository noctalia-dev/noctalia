#include "system/weather_service.h"

#include "config/config_service.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "json.hpp"
#include "net/http_client.h"
#include "time/time_format.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace {

  constexpr Logger kLog("weather");
  constexpr std::size_t kForecastDays = 7;
  constexpr std::size_t kForecastHours = kForecastDays * 24;

  using Clock = std::chrono::system_clock;

  std::chrono::system_clock::time_point fromUnixSeconds(std::int64_t value) {
    return Clock::time_point{std::chrono::seconds{value}};
  }

  std::int64_t toUnixSeconds(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
  }

  bool isIsoDate(std::string_view text) {
    return text.size() == 10
        && std::isdigit(static_cast<unsigned char>(text[0])) != 0
        && std::isdigit(static_cast<unsigned char>(text[1])) != 0
        && std::isdigit(static_cast<unsigned char>(text[2])) != 0
        && std::isdigit(static_cast<unsigned char>(text[3])) != 0
        && text[4] == '-'
        && std::isdigit(static_cast<unsigned char>(text[5])) != 0
        && std::isdigit(static_cast<unsigned char>(text[6])) != 0
        && text[7] == '-'
        && std::isdigit(static_cast<unsigned char>(text[8])) != 0
        && std::isdigit(static_cast<unsigned char>(text[9])) != 0;
  }

  bool isIsoHour(std::string_view text) {
    return text.size() == 16
        && isIsoDate(text.substr(0, 10))
        && text[10] == 'T'
        && std::isdigit(static_cast<unsigned char>(text[11])) != 0
        && std::isdigit(static_cast<unsigned char>(text[12])) != 0
        && text[13] == ':'
        && std::isdigit(static_cast<unsigned char>(text[14])) != 0
        && std::isdigit(static_cast<unsigned char>(text[15])) != 0;
  }

  std::string todayIsoForOffset(std::int32_t utcOffsetSeconds) {
    const auto shiftedNow = Clock::now() + std::chrono::seconds{utcOffsetSeconds};
    const std::time_t time = Clock::to_time_t(shiftedNow);
    std::tm tm{};
    gmtime_r(&time, &tm);

    return formatStrftime("%Y-%m-%d", tm);
  }

  std::string currentHourIsoForOffset(std::int32_t utcOffsetSeconds) {
    const auto shiftedNow = Clock::now() + std::chrono::seconds{utcOffsetSeconds};
    const std::time_t time = Clock::to_time_t(shiftedNow);
    std::tm tm{};
    gmtime_r(&time, &tm);

    return formatStrftime("%Y-%m-%dT%H:00", tm);
  }

  bool dropPastForecastDays(WeatherSnapshot& snapshot) {
    if (!snapshot.valid || snapshot.forecastDays.empty()) {
      return false;
    }

    const std::string todayIso = todayIsoForOffset(snapshot.utcOffsetSeconds);
    if (!isIsoDate(todayIso)) {
      return false;
    }

    const auto oldSize = snapshot.forecastDays.size();
    std::erase_if(snapshot.forecastDays, [&todayIso](const WeatherForecastDay& day) {
      return isIsoDate(day.dateIso) && day.dateIso < todayIso;
    });
    return snapshot.forecastDays.size() != oldSize;
  }

  bool dropPastForecastHours(WeatherSnapshot& snapshot) {
    if (!snapshot.valid || snapshot.forecastHours.empty()) {
      return false;
    }

    const std::string currentHourIso = currentHourIsoForOffset(snapshot.utcOffsetSeconds);
    if (!isIsoHour(currentHourIso)) {
      return false;
    }

    const auto oldSize = snapshot.forecastHours.size();
    std::erase_if(snapshot.forecastHours, [&currentHourIso](const WeatherForecastHour& hour) {
      return isIsoHour(hour.timeIso) && hour.timeIso < currentHourIso;
    });
    return snapshot.forecastHours.size() != oldSize;
  }

  double readNumber(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_number()) {
      throw std::runtime_error(std::string("missing numeric key: ") + key);
    }
    return it->get<double>();
  }

  double readOptionalNumber(const nlohmann::json& json, const char* key, double fallback = 0.0) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_number()) {
      return fallback;
    }
    return it->get<double>();
  }

  std::int32_t readInt(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_number_integer()) {
      throw std::runtime_error(std::string("missing integer key: ") + key);
    }
    return it->get<std::int32_t>();
  }

  std::int32_t readOptionalInt(const nlohmann::json& json, const char* key, std::int32_t fallback = 0) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_number_integer()) {
      return fallback;
    }
    return it->get<std::int32_t>();
  }

  std::string readString(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_string()) {
      return {};
    }
    return it->get<std::string>();
  }

  bool readBool(const nlohmann::json& json, const char* key, bool fallback = false) {
    const auto it = json.find(key);
    if (it == json.end()) {
      return fallback;
    }
    if (it->is_boolean()) {
      return it->get<bool>();
    }
    if (it->is_number_integer()) {
      return it->get<int>() != 0;
    }
    return fallback;
  }

  bool readBoolValue(const nlohmann::json& value, bool fallback = false) {
    if (value.is_boolean()) {
      return value.get<bool>();
    }
    if (value.is_number_integer()) {
      return value.get<int>() != 0;
    }
    return fallback;
  }

  nlohmann::json currentUnitsToJson(const WeatherCurrentUnits& units) {
    return nlohmann::json{
        {"time", units.time},
        {"interval", units.interval},
        {"temperature", units.temperature},
        {"wind_speed", units.windSpeed},
        {"wind_direction", units.windDirection},
        {"is_day", units.isDay},
        {"weather_code", units.weatherCode},
        {"uv_index", units.uvIndex},
    };
  }

  nlohmann::json dailyUnitsToJson(const WeatherDailyUnits& units) {
    return nlohmann::json{
        {"time", units.time},
        {"temperature_max", units.temperatureMax},
        {"temperature_min", units.temperatureMin},
        {"weather_code", units.weatherCode},
        {"sunrise", units.sunrise},
        {"sunset", units.sunset},
    };
  }

  nlohmann::json hourlyUnitsToJson(const WeatherHourlyUnits& units) {
    return nlohmann::json{
        {"time", units.time},
        {"temperature", units.temperature},
        {"relative_humidity", units.relativeHumidity},
        {"precipitation_probability", units.precipitationProbability},
        {"weather_code", units.weatherCode},
        {"is_day", units.isDay},
        {"wind_speed", units.windSpeed},
    };
  }

  WeatherCurrentUnits currentUnitsFromJson(const nlohmann::json& json) {
    WeatherCurrentUnits units;
    units.time = readString(json, "time");
    units.interval = readString(json, "interval");
    units.temperature = readString(json, "temperature");
    units.windSpeed = readString(json, "wind_speed");
    units.windDirection = readString(json, "wind_direction");
    units.isDay = readString(json, "is_day");
    units.weatherCode = readString(json, "weather_code");
    units.uvIndex = readString(json, "uv_index");
    return units;
  }

  WeatherDailyUnits dailyUnitsFromJson(const nlohmann::json& json) {
    WeatherDailyUnits units;
    units.time = readString(json, "time");
    units.temperatureMax = readString(json, "temperature_max");
    units.temperatureMin = readString(json, "temperature_min");
    units.weatherCode = readString(json, "weather_code");
    units.sunrise = readString(json, "sunrise");
    units.sunset = readString(json, "sunset");
    return units;
  }

  WeatherHourlyUnits hourlyUnitsFromJson(const nlohmann::json& json) {
    WeatherHourlyUnits units;
    units.time = readString(json, "time");
    units.temperature = readString(json, "temperature");
    units.relativeHumidity = readString(json, "relative_humidity");
    units.precipitationProbability = readString(json, "precipitation_probability");
    units.weatherCode = readString(json, "weather_code");
    units.isDay = readString(json, "is_day");
    units.windSpeed = readString(json, "wind_speed");
    return units;
  }

} // namespace

WeatherService::WeatherService(ConfigService& configService, HttpClient& httpClient)
    : m_configService(configService), m_httpClient(httpClient) {}

void WeatherService::initialize() {
  m_activeConfig = m_configService.config().weather;
  m_configService.addReloadCallback([this]() { onConfigReload(); });
  loadCache();
  requestRefresh();
}

void WeatherService::addChangeCallback(ChangeCallback callback) { m_callbacks.push_back(std::move(callback)); }

void WeatherService::setLocation(
    std::optional<WeatherCoordinates> coordinates, std::string name, std::string sourceLabel
) {
  m_locationName = std::move(name);
  m_locationSource = std::move(sourceLabel);

  if (!coordinates.has_value()) {
    if (m_hasLocation || m_snapshot.valid || !m_error.empty()) {
      m_hasLocation = false;
      clearState();
      notifyChanged();
    }
    return;
  }

  constexpr double kEpsilon = 1e-6;
  const bool coordinatesChanged = !m_hasLocation
      || std::abs(m_resolvedLatitude - coordinates->latitude) > kEpsilon
      || std::abs(m_resolvedLongitude - coordinates->longitude) > kEpsilon;

  m_resolvedLatitude = coordinates->latitude;
  m_resolvedLongitude = coordinates->longitude;
  m_hasLocation = true;

  if (coordinatesChanged) {
    requestRefresh();
  }
  notifyChanged();
}

int WeatherService::pollTimeoutMs() const {
  if (!m_activeConfig.enabled || m_requestKind != RequestKind::None) {
    return -1;
  }
  if (m_refreshQueued) {
    return 0;
  }
  if (!m_hasLocation) {
    return -1;
  }

  const auto now = Clock::now();
  if (m_nextRefreshAt <= now) {
    return 0;
  }
  return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(m_nextRefreshAt - now).count());
}

void WeatherService::tick() {
  if (m_requestKind != RequestKind::None) {
    return;
  }
  if (!m_activeConfig.enabled) {
    return;
  }
  if (!m_hasLocation) {
    m_refreshQueued = false;
    if (m_snapshot.valid || !m_error.empty()) {
      clearState();
      notifyChanged();
    }
    return;
  }

  const auto now = Clock::now();
  if (!m_refreshQueued && now < m_nextRefreshAt) {
    return;
  }
  m_refreshQueued = false;

  startWeatherFetch();
}

void WeatherService::requestRefresh() {
  m_refreshQueued = true;
  m_nextRefreshAt = Clock::time_point{};
}

bool WeatherService::enabled() const noexcept { return m_activeConfig.enabled; }

bool WeatherService::locationConfigured() const noexcept { return m_hasLocation; }

bool WeatherService::useImperial() const noexcept { return m_activeConfig.unit == "imperial"; }

double WeatherService::displayTemperature(double celsius) const noexcept {
  if (!useImperial()) {
    return celsius;
  }
  return 32.0 + (celsius * 1.8);
}

const char* WeatherService::displayTemperatureUnit() const noexcept { return useImperial() ? "\u00b0F" : "\u00b0C"; }

std::string WeatherService::glyphForCode(std::int32_t code, bool isDay) {
  if (code == 0) {
    return isDay ? "weather-sun" : "weather-moon";
  }
  if (code == 1 || code == 2) {
    return isDay ? "weather-cloud-sun" : "weather-moon-stars";
  }
  if (code == 3) {
    return "weather-cloud";
  }
  if (code >= 45 && code <= 48) {
    return "weather-cloud-haze";
  }
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    return "weather-cloud-rain";
  }
  if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) {
    return "weather-cloud-snow";
  }
  if (code >= 95 && code <= 99) {
    return "weather-cloud-lightning";
  }
  return "weather-cloud";
}

std::string WeatherService::shortDescriptionForCode(std::int32_t code) {
  if (code == 0) {
    return i18n::tr("weather.conditions.short.clear");
  }
  if (code == 1) {
    return i18n::tr("weather.conditions.short.mostly-clear");
  }
  if (code == 2) {
    return i18n::tr("weather.conditions.short.cloudy");
  }
  if (code == 3) {
    return i18n::tr("weather.conditions.short.overcast");
  }
  if (code == 45 || code == 48) {
    return i18n::tr("weather.conditions.short.fog");
  }
  if (code >= 51 && code <= 67) {
    return i18n::tr("weather.conditions.short.drizzle");
  }
  if (code >= 71 && code <= 77) {
    return i18n::tr("weather.conditions.short.snow");
  }
  if (code >= 80 && code <= 82) {
    return i18n::tr("weather.conditions.short.showers");
  }
  if (code >= 85 && code <= 86) {
    return i18n::tr("weather.conditions.short.snow-showers");
  }
  if (code >= 95 && code <= 99) {
    return i18n::tr("weather.conditions.short.storm");
  }
  return i18n::tr("weather.conditions.short.weather");
}

std::string WeatherService::descriptionForCode(std::int32_t code) {
  if (code == 0) {
    return i18n::tr("weather.conditions.full.clear-sky");
  }
  if (code == 1) {
    return i18n::tr("weather.conditions.full.mainly-clear");
  }
  if (code == 2) {
    return i18n::tr("weather.conditions.full.partly-cloudy");
  }
  if (code == 3) {
    return i18n::tr("weather.conditions.full.overcast");
  }
  if (code == 45 || code == 48) {
    return i18n::tr("weather.conditions.full.fog");
  }
  if (code >= 51 && code <= 67) {
    return i18n::tr("weather.conditions.full.drizzle");
  }
  if (code >= 71 && code <= 77) {
    return i18n::tr("weather.conditions.full.snow");
  }
  if (code >= 80 && code <= 82) {
    return i18n::tr("weather.conditions.full.rain-showers");
  }
  if (code >= 85 && code <= 86) {
    return i18n::tr("weather.conditions.full.snow-showers");
  }
  if (code >= 95 && code <= 99) {
    return i18n::tr("weather.conditions.full.thunderstorm");
  }
  return i18n::tr("weather.conditions.full.unknown");
}

void WeatherService::onConfigReload() {
  const WeatherConfig previousConfig = m_activeConfig;
  const WeatherConfig nextConfig = m_configService.config().weather;
  if (previousConfig == nextConfig) {
    return;
  }

  m_activeConfig = nextConfig;
  m_error.clear();
  if (!m_activeConfig.enabled) {
    clearState();
    notifyChanged();
    return;
  }

  if (!previousConfig.enabled) {
    // Re-enabled: reload cache and refresh for the current location.
    loadCache();
    requestRefresh();
  } else if (m_snapshot.valid) {
    m_nextRefreshAt = m_snapshot.fetchedAt + std::chrono::minutes(std::max(5, m_activeConfig.refreshMinutes));
  } else {
    requestRefresh();
  }
  notifyChanged();
}

void WeatherService::clearState() {
  ++m_requestSerial;
  m_loading = false;
  m_error.clear();
  m_requestKind = RequestKind::None;
  m_snapshot = WeatherSnapshot{};
  m_nextRefreshAt = Clock::time_point{};
}

void WeatherService::notifyChanged() {
  for (const auto& callback : m_callbacks) {
    callback();
  }
}

void WeatherService::startWeatherFetch() {
  std::error_code ec;
  std::filesystem::create_directories(transportCacheDir(), ec);
  const auto path = transportCacheDir() / "forecast.json";
  const std::string url = std::format(
      "https://api.open-meteo.com/v1/forecast?latitude={}&longitude={}"
      "&current=temperature_2m,wind_speed_10m,wind_direction_10m,weather_code,is_day,uv_index"
      "&hourly=temperature_2m,relative_humidity_2m,precipitation_probability,weather_code,is_day,wind_speed_10m"
      "&daily=temperature_2m_max,temperature_2m_min,weather_code,sunrise,sunset"
      "&forecast_days={}&forecast_hours={}&timezone=auto",
      formatCoordinate(m_resolvedLatitude), formatCoordinate(m_resolvedLongitude), kForecastDays, kForecastHours
  );
  const std::uint64_t serial = ++m_requestSerial;
  m_loading = true;
  m_error.clear();
  m_requestKind = RequestKind::FetchWeather;
  notifyChanged();
  m_httpClient.download(url, path, [this, path, serial](bool success) {
    handleWeatherResponse(path, success, serial);
  });
}

void WeatherService::handleWeatherResponse(const std::filesystem::path& path, bool success, std::uint64_t serial) {
  if (serial != m_requestSerial || !m_activeConfig.enabled) {
    return;
  }
  m_requestKind = RequestKind::None;
  m_loading = false;
  if (!success) {
    m_error = i18n::tr("weather.errors.fetch-failed");
    scheduleRetryAfterFailure();
    dropPastForecastHours(m_snapshot);
    dropPastForecastDays(m_snapshot);
    notifyChanged();
    return;
  }

  try {
    std::ifstream file(path);
    const auto json = nlohmann::json::parse(file);
    const auto& current = json.at("current");
    const auto& hourly = json.at("hourly");
    const auto& daily = json.at("daily");
    const auto& hourTimes = hourly.at("time");
    const auto& hourTemps = hourly.at("temperature_2m");
    const auto& hourHumidity = hourly.at("relative_humidity_2m");
    const auto& hourPrecipitationProbability = hourly.at("precipitation_probability");
    const auto& hourCodes = hourly.at("weather_code");
    const auto& hourIsDay = hourly.at("is_day");
    const auto& hourWindSpeeds = hourly.at("wind_speed_10m");
    const auto& dates = daily.at("time");
    const auto& tempsMax = daily.at("temperature_2m_max");
    const auto& tempsMin = daily.at("temperature_2m_min");
    const auto& codes = daily.at("weather_code");
    const auto& sunrises = daily.at("sunrise");
    const auto& sunsets = daily.at("sunset");

    WeatherSnapshot next;
    next.valid = true;
    next.locationName = m_locationName;
    next.sourceLabel = m_locationSource;
    next.latitude = m_resolvedLatitude;
    next.longitude = m_resolvedLongitude;
    next.generationTimeMs = readOptionalNumber(json, "generationtime_ms");
    next.utcOffsetSeconds = readOptionalInt(json, "utc_offset_seconds");
    next.timezone = readString(json, "timezone");
    next.timezoneAbbreviation = readString(json, "timezone_abbreviation");
    next.elevationM = readOptionalNumber(json, "elevation");
    if (const auto it = json.find("current_units"); it != json.end() && it->is_object()) {
      next.currentUnits.time = readString(*it, "time");
      next.currentUnits.interval = readString(*it, "interval");
      next.currentUnits.temperature = readString(*it, "temperature_2m");
      next.currentUnits.windSpeed = readString(*it, "wind_speed_10m");
      next.currentUnits.windDirection = readString(*it, "wind_direction_10m");
      next.currentUnits.isDay = readString(*it, "is_day");
      next.currentUnits.weatherCode = readString(*it, "weather_code");
      next.currentUnits.uvIndex = readString(*it, "uv_index");
    }
    if (const auto it = json.find("daily_units"); it != json.end() && it->is_object()) {
      next.dailyUnits.time = readString(*it, "time");
      next.dailyUnits.temperatureMax = readString(*it, "temperature_2m_max");
      next.dailyUnits.temperatureMin = readString(*it, "temperature_2m_min");
      next.dailyUnits.weatherCode = readString(*it, "weather_code");
      next.dailyUnits.sunrise = readString(*it, "sunrise");
      next.dailyUnits.sunset = readString(*it, "sunset");
    }
    if (const auto it = json.find("hourly_units"); it != json.end() && it->is_object()) {
      next.hourlyUnits.time = readString(*it, "time");
      next.hourlyUnits.temperature = readString(*it, "temperature_2m");
      next.hourlyUnits.relativeHumidity = readString(*it, "relative_humidity_2m");
      next.hourlyUnits.precipitationProbability = readString(*it, "precipitation_probability");
      next.hourlyUnits.weatherCode = readString(*it, "weather_code");
      next.hourlyUnits.isDay = readString(*it, "is_day");
      next.hourlyUnits.windSpeed = readString(*it, "wind_speed_10m");
    }
    next.current.timeIso = readString(current, "time");
    next.current.intervalSeconds = readOptionalInt(current, "interval");
    next.current.temperatureC = readNumber(current, "temperature_2m");
    next.current.windSpeedKmh = readOptionalNumber(current, "wind_speed_10m");
    next.current.windDirectionDeg = readOptionalInt(current, "wind_direction_10m");
    next.current.isDay = readBool(current, "is_day", true);
    next.current.weatherCode = readInt(current, "weather_code");
    next.current.uvIndex = readOptionalNumber(current, "uv_index");
    next.fetchedAt = Clock::now();

    const std::size_t hourCount = std::min(
        {hourTimes.size(), hourTemps.size(), hourHumidity.size(), hourPrecipitationProbability.size(), hourCodes.size(),
         hourIsDay.size(), hourWindSpeeds.size(), kForecastHours}
    );
    next.forecastHours.reserve(hourCount);
    for (std::size_t i = 0; i < hourCount; ++i) {
      if (!hourTimes[i].is_string()
          || !hourTemps[i].is_number()
          || !hourHumidity[i].is_number_integer()
          || !hourPrecipitationProbability[i].is_number_integer()
          || !hourCodes[i].is_number_integer()
          || (!hourIsDay[i].is_boolean() && !hourIsDay[i].is_number_integer())
          || !hourWindSpeeds[i].is_number()) {
        continue;
      }
      next.forecastHours.push_back(
          WeatherForecastHour{
              .timeIso = hourTimes[i].get<std::string>(),
              .weatherCode = hourCodes[i].get<std::int32_t>(),
              .temperatureC = hourTemps[i].get<double>(),
              .relativeHumidityPercent = hourHumidity[i].get<std::int32_t>(),
              .precipitationProbabilityPercent = hourPrecipitationProbability[i].get<std::int32_t>(),
              .isDay = readBoolValue(hourIsDay[i], true),
              .windSpeedKmh = hourWindSpeeds[i].get<double>(),
          }
      );
    }
    dropPastForecastHours(next);

    const std::size_t count = std::min(
        {dates.size(), tempsMax.size(), tempsMin.size(), codes.size(), sunrises.size(), sunsets.size(), kForecastDays}
    );
    next.forecastDays.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      if (!dates[i].is_string() || !tempsMax[i].is_number() || !tempsMin[i].is_number() || !codes[i].is_number()) {
        continue;
      }
      next.forecastDays.push_back(
          WeatherForecastDay{
              .dateIso = dates[i].get<std::string>(),
              .weatherCode = codes[i].get<std::int32_t>(),
              .temperatureMaxC = tempsMax[i].get<double>(),
              .temperatureMinC = tempsMin[i].get<double>(),
              .sunriseIso = sunrises[i].is_string() ? sunrises[i].get<std::string>() : std::string{},
              .sunsetIso = sunsets[i].is_string() ? sunsets[i].get<std::string>() : std::string{},
          }
      );
    }
    dropPastForecastDays(next);

    m_snapshot = std::move(next);
    m_error.clear();
    m_nextRefreshAt = Clock::now() + std::chrono::minutes(std::max(5, m_activeConfig.refreshMinutes));
    saveCache();
    notifyChanged();
  } catch (const std::exception& e) {
    m_error = i18n::tr("weather.errors.parse-weather");
    scheduleRetryAfterFailure();
    kLog.warn("{}: {}", m_error, e.what());
    notifyChanged();
  }
}

void WeatherService::scheduleRetryAfterFailure() {
  m_refreshQueued = false;
  m_nextRefreshAt = Clock::now() + std::chrono::minutes(std::max(5, m_activeConfig.refreshMinutes));
}

bool WeatherService::coordinatesValid() const noexcept {
  return std::isfinite(m_resolvedLatitude)
      && std::isfinite(m_resolvedLongitude)
      && m_resolvedLatitude >= -90.0
      && m_resolvedLatitude <= 90.0
      && m_resolvedLongitude >= -180.0
      && m_resolvedLongitude <= 180.0;
}

std::filesystem::path WeatherService::transportCacheDir() { return std::filesystem::path("/tmp") / "noctalia-weather"; }

std::filesystem::path WeatherService::stateCacheFilePath() {
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] != '\0') {
    return std::filesystem::path(xdg) / "noctalia" / "weather.json";
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    return std::filesystem::path(home) / ".cache" / "noctalia" / "weather.json";
  }
  return std::filesystem::path("/tmp") / "noctalia-weather-cache.json";
}

std::string WeatherService::formatCoordinate(double value) { return std::format("{:.4f}", value); }

void WeatherService::loadCache() {
  clearState();
  const auto path = stateCacheFilePath();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return;
  }

  try {
    std::ifstream file(path);
    const auto json = nlohmann::json::parse(file);

    const auto& snapshot = json.at("snapshot");
    if (!readBool(snapshot, "valid")) {
      return;
    }

    m_snapshot.valid = true;
    m_snapshot.locationName = readString(snapshot, "location_name");
    m_snapshot.sourceLabel = readString(snapshot, "source_label");
    m_snapshot.latitude = readNumber(snapshot, "latitude");
    m_snapshot.longitude = readNumber(snapshot, "longitude");
    m_snapshot.generationTimeMs = readOptionalNumber(snapshot, "generation_time_ms");
    m_snapshot.utcOffsetSeconds = readOptionalInt(snapshot, "utc_offset_seconds");
    m_snapshot.timezone = readString(snapshot, "timezone");
    m_snapshot.timezoneAbbreviation = readString(snapshot, "timezone_abbreviation");
    m_snapshot.elevationM = readOptionalNumber(snapshot, "elevation_m");
    if (const auto it = snapshot.find("current_units"); it != snapshot.end() && it->is_object()) {
      m_snapshot.currentUnits = currentUnitsFromJson(*it);
    }
    if (const auto it = snapshot.find("daily_units"); it != snapshot.end() && it->is_object()) {
      m_snapshot.dailyUnits = dailyUnitsFromJson(*it);
    }
    if (const auto it = snapshot.find("hourly_units"); it != snapshot.end() && it->is_object()) {
      m_snapshot.hourlyUnits = hourlyUnitsFromJson(*it);
    }
    if (const auto it = snapshot.find("current"); it != snapshot.end() && it->is_object()) {
      m_snapshot.current.timeIso = readString(*it, "time_iso");
      m_snapshot.current.intervalSeconds = readOptionalInt(*it, "interval_seconds");
      m_snapshot.current.temperatureC = readOptionalNumber(*it, "temperature_c");
      m_snapshot.current.windSpeedKmh = readOptionalNumber(*it, "wind_speed_kmh");
      m_snapshot.current.windDirectionDeg = readOptionalInt(*it, "wind_direction_deg");
      m_snapshot.current.isDay = readBool(*it, "is_day", true);
      m_snapshot.current.weatherCode = readOptionalInt(*it, "weather_code");
      m_snapshot.current.uvIndex = readOptionalNumber(*it, "uv_index");
    }
    if (const auto it = snapshot.find("forecast_hours"); it != snapshot.end() && it->is_array()) {
      m_snapshot.forecastHours.clear();
      for (const auto& item : *it) {
        if (!item.is_object()) {
          continue;
        }
        m_snapshot.forecastHours.push_back(
            WeatherForecastHour{
                .timeIso = readString(item, "time_iso"),
                .weatherCode = readOptionalInt(item, "weather_code"),
                .temperatureC = readOptionalNumber(item, "temperature_c"),
                .relativeHumidityPercent = readOptionalInt(item, "relative_humidity_percent"),
                .precipitationProbabilityPercent = readOptionalInt(item, "precipitation_probability_percent"),
                .isDay = readBool(item, "is_day", true),
                .windSpeedKmh = readOptionalNumber(item, "wind_speed_kmh"),
            }
        );
      }
    }
    if (const auto it = snapshot.find("forecast_days"); it != snapshot.end() && it->is_array()) {
      m_snapshot.forecastDays.clear();
      for (const auto& item : *it) {
        if (!item.is_object()) {
          continue;
        }
        m_snapshot.forecastDays.push_back(
            WeatherForecastDay{
                .dateIso = readString(item, "date_iso"),
                .weatherCode = readOptionalInt(item, "weather_code"),
                .temperatureMaxC = readOptionalNumber(item, "temperature_max_c"),
                .temperatureMinC = readOptionalNumber(item, "temperature_min_c"),
                .sunriseIso = readString(item, "sunrise_iso"),
                .sunsetIso = readString(item, "sunset_iso"),
            }
        );
      }
    }
    m_snapshot.fetchedAt = fromUnixSeconds(readOptionalInt(snapshot, "fetched_at"));
    dropPastForecastHours(m_snapshot);
    dropPastForecastDays(m_snapshot);

    m_resolvedLatitude = m_snapshot.latitude;
    m_resolvedLongitude = m_snapshot.longitude;
    m_locationName = m_snapshot.locationName;
    m_locationSource = m_snapshot.sourceLabel;
    m_hasLocation = coordinatesValid();
    m_nextRefreshAt = m_snapshot.fetchedAt + std::chrono::minutes(std::max(5, m_activeConfig.refreshMinutes));

    kLog.info("loaded cached weather data");
  } catch (const std::exception& e) {
    kLog.warn("failed to load weather cache: {}", e.what());
  }
}

void WeatherService::saveCache() const {
  if (!m_snapshot.valid) {
    return;
  }

  const auto path = stateCacheFilePath();
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);

  nlohmann::json json{
      {"snapshot",
       {
           {"valid", true},
           {"location_name", m_snapshot.locationName},
           {"source_label", m_snapshot.sourceLabel},
           {"latitude", m_snapshot.latitude},
           {"longitude", m_snapshot.longitude},
           {"generation_time_ms", m_snapshot.generationTimeMs},
           {"utc_offset_seconds", m_snapshot.utcOffsetSeconds},
           {"timezone", m_snapshot.timezone},
           {"timezone_abbreviation", m_snapshot.timezoneAbbreviation},
           {"elevation_m", m_snapshot.elevationM},
           {"current_units", currentUnitsToJson(m_snapshot.currentUnits)},
           {"daily_units", dailyUnitsToJson(m_snapshot.dailyUnits)},
           {"hourly_units", hourlyUnitsToJson(m_snapshot.hourlyUnits)},
           {"current",
            {
                {"time_iso", m_snapshot.current.timeIso},
                {"interval_seconds", m_snapshot.current.intervalSeconds},
                {"temperature_c", m_snapshot.current.temperatureC},
                {"wind_speed_kmh", m_snapshot.current.windSpeedKmh},
                {"wind_direction_deg", m_snapshot.current.windDirectionDeg},
                {"is_day", m_snapshot.current.isDay},
                {"weather_code", m_snapshot.current.weatherCode},
                {"uv_index", m_snapshot.current.uvIndex},
            }},
           {"forecast_hours", nlohmann::json::array()},
           {"forecast_days", nlohmann::json::array()},
           {"fetched_at", toUnixSeconds(m_snapshot.fetchedAt)},
       }}
  };

  for (const auto& hour : m_snapshot.forecastHours) {
    json["snapshot"]["forecast_hours"].push_back({
        {"time_iso", hour.timeIso},
        {"weather_code", hour.weatherCode},
        {"temperature_c", hour.temperatureC},
        {"relative_humidity_percent", hour.relativeHumidityPercent},
        {"precipitation_probability_percent", hour.precipitationProbabilityPercent},
        {"is_day", hour.isDay},
        {"wind_speed_kmh", hour.windSpeedKmh},
    });
  }

  for (const auto& day : m_snapshot.forecastDays) {
    json["snapshot"]["forecast_days"].push_back({
        {"date_iso", day.dateIso},
        {"weather_code", day.weatherCode},
        {"temperature_max_c", day.temperatureMaxC},
        {"temperature_min_c", day.temperatureMinC},
        {"sunrise_iso", day.sunriseIso},
        {"sunset_iso", day.sunsetIso},
    });
  }

  try {
    std::ofstream file(path);
    file << json.dump(2);
  } catch (const std::exception& e) {
    kLog.warn("failed to save weather cache: {}", e.what());
  }
}
