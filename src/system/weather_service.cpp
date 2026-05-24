#include "system/weather_service.h"

#include "core/log.h"
#include "i18n/i18n.h"
#include "json.hpp"
#include "net/http_client.h"
#include "time/time_format.h"
#include "util/string_utils.h"

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

  using Clock = std::chrono::system_clock;

  bool weatherConfigEqual(const WeatherConfig& lhs, const WeatherConfig& rhs) {
    return lhs.enabled == rhs.enabled && lhs.autoLocate == rhs.autoLocate && lhs.effects == rhs.effects &&
           lhs.address == rhs.address && lhs.refreshMinutes == rhs.refreshMinutes && lhs.unit == rhs.unit;
  }

  bool weatherLocationConfigEqual(const WeatherConfig& lhs, const WeatherConfig& rhs) {
    return lhs.enabled == rhs.enabled && lhs.autoLocate == rhs.autoLocate && lhs.address == rhs.address;
  }

  std::chrono::system_clock::time_point fromUnixSeconds(std::int64_t value) {
    return Clock::time_point{std::chrono::seconds{value}};
  }

  std::int64_t toUnixSeconds(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
  }

  bool isIsoDate(std::string_view text) {
    return text.size() == 10 && std::isdigit(static_cast<unsigned char>(text[0])) != 0 &&
           std::isdigit(static_cast<unsigned char>(text[1])) != 0 &&
           std::isdigit(static_cast<unsigned char>(text[2])) != 0 &&
           std::isdigit(static_cast<unsigned char>(text[3])) != 0 && text[4] == '-' &&
           std::isdigit(static_cast<unsigned char>(text[5])) != 0 &&
           std::isdigit(static_cast<unsigned char>(text[6])) != 0 && text[7] == '-' &&
           std::isdigit(static_cast<unsigned char>(text[8])) != 0 &&
           std::isdigit(static_cast<unsigned char>(text[9])) != 0;
  }

  std::string todayIsoForOffset(std::int32_t utcOffsetSeconds) {
    const auto shiftedNow = Clock::now() + std::chrono::seconds{utcOffsetSeconds};
    const std::time_t time = Clock::to_time_t(shiftedNow);
    std::tm tm{};
    gmtime_r(&time, &tm);

    return formatStrftime("%Y-%m-%d", tm);
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
    snapshot.forecastDays.erase(
        std::remove_if(
            snapshot.forecastDays.begin(), snapshot.forecastDays.end(),
            [&todayIso](const WeatherForecastDay& day) { return isIsoDate(day.dateIso) && day.dateIso < todayIso; }
        ),
        snapshot.forecastDays.end()
    );
    return snapshot.forecastDays.size() != oldSize;
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

  nlohmann::json currentUnitsToJson(const WeatherCurrentUnits& units) {
    return nlohmann::json{
        {"time", units.time},
        {"interval", units.interval},
        {"temperature", units.temperature},
        {"wind_speed", units.windSpeed},
        {"wind_direction", units.windDirection},
        {"is_day", units.isDay},
        {"weather_code", units.weatherCode},
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

  WeatherCurrentUnits currentUnitsFromJson(const nlohmann::json& json) {
    WeatherCurrentUnits units;
    units.time = readString(json, "time");
    units.interval = readString(json, "interval");
    units.temperature = readString(json, "temperature");
    units.windSpeed = readString(json, "wind_speed");
    units.windDirection = readString(json, "wind_direction");
    units.isDay = readString(json, "is_day");
    units.weatherCode = readString(json, "weather_code");
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

} // namespace

WeatherService::WeatherService(ConfigService& configService, HttpClient& httpClient)
    : m_configService(configService), m_httpClient(httpClient) {}

void WeatherService::initialize() {
  m_activeConfig = m_configService.config().weather;
  m_configService.addReloadCallback([this]() { onConfigReload(); });
  loadCache();
  requestRefresh(!m_snapshot.valid);
}

void WeatherService::addChangeCallback(ChangeCallback callback) { m_callbacks.push_back(std::move(callback)); }

int WeatherService::pollTimeoutMs() const {
  if (!m_activeConfig.enabled || m_requestKind != RequestKind::None) {
    return -1;
  }
  if (m_refreshQueued) {
    return 0;
  }
  if (!locationConfigured()) {
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
  if (!locationConfigured()) {
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

  if (m_activeConfig.autoLocate) {
    startGeolocate();
    return;
  }

  if (!m_locationResolved || m_resolvedAddress != m_activeConfig.address) {
    startAddressGeocode();
    return;
  }

  startWeatherFetch();
}

void WeatherService::requestRefresh(bool resetLocation) {
  if (resetLocation) {
    m_locationResolved = false;
    m_resolvedAddress.clear();
    m_resolvedLocationName.clear();
    m_resolvedLatitude = 0.0;
    m_resolvedLongitude = 0.0;
  }
  m_refreshQueued = true;
  m_nextRefreshAt = Clock::time_point{};
}

bool WeatherService::enabled() const noexcept { return m_activeConfig.enabled; }

bool WeatherService::locationConfigured() const noexcept {
  return m_activeConfig.autoLocate || !m_activeConfig.address.empty();
}

std::optional<WeatherCoordinates> WeatherService::resolvedCoordinates() const noexcept {
  if (!hasResolvedLocation()) {
    return std::nullopt;
  }
  return WeatherCoordinates{.latitude = m_resolvedLatitude, .longitude = m_resolvedLongitude};
}

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
  if (weatherConfigEqual(previousConfig, nextConfig)) {
    return;
  }

  m_activeConfig = nextConfig;
  m_error.clear();
  if (!m_activeConfig.enabled || !locationConfigured()) {
    clearState();
    notifyChanged();
    return;
  }

  if (weatherLocationConfigEqual(previousConfig, m_activeConfig)) {
    if (m_snapshot.valid) {
      m_nextRefreshAt = m_snapshot.fetchedAt + std::chrono::minutes(std::max(5, m_activeConfig.refreshMinutes));
    } else {
      requestRefresh(false);
    }
    if (previousConfig.unit != m_activeConfig.unit || previousConfig.effects != m_activeConfig.effects) {
      notifyChanged();
    }
    return;
  }

  loadCache();
  requestRefresh(!m_snapshot.valid);
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

void WeatherService::startGeolocate() {
  std::error_code ec;
  std::filesystem::create_directories(transportCacheDir(), ec);
  const auto path = transportCacheDir() / "geolocate.json";
  const std::uint64_t serial = ++m_requestSerial;
  m_loading = true;
  m_error.clear();
  m_requestKind = RequestKind::Geolocate;
  notifyChanged();
  m_httpClient.download("https://api.noctalia.dev/geolocate", path, [this, path, serial](bool success) {
    handleLocationResponse(path, true, success, serial);
  });
}

void WeatherService::startAddressGeocode() {
  std::error_code ec;
  std::filesystem::create_directories(transportCacheDir(), ec);
  const auto path = transportCacheDir() / "geocode.json";
  const std::string url = "https://api.noctalia.dev/geocode?city=" + StringUtils::urlEncode(m_activeConfig.address);
  const std::uint64_t serial = ++m_requestSerial;
  m_loading = true;
  m_error.clear();
  m_requestKind = RequestKind::GeocodeAddress;
  notifyChanged();
  m_httpClient.download(url, path, [this, path, serial](bool success) {
    handleLocationResponse(path, false, success, serial);
  });
}

void WeatherService::startWeatherFetch() {
  std::error_code ec;
  std::filesystem::create_directories(transportCacheDir(), ec);
  const auto path = transportCacheDir() / "forecast.json";
  const std::string url = std::format(
      "https://api.open-meteo.com/v1/forecast?latitude={}&longitude={}"
      "&current_weather=true"
      "&daily=temperature_2m_max,temperature_2m_min,weathercode,sunrise,sunset"
      "&forecast_days={}&timezone=auto",
      formatCoordinate(m_resolvedLatitude), formatCoordinate(m_resolvedLongitude), kForecastDays
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

void WeatherService::handleLocationResponse(
    const std::filesystem::path& path, bool autoLocated, bool success, std::uint64_t serial
) {
  if (serial != m_requestSerial || !m_activeConfig.enabled) {
    return;
  }
  m_requestKind = RequestKind::None;
  if (!success) {
    m_error = autoLocated ? i18n::tr("weather.errors.ip-geolocation-failed")
                          : i18n::tr("weather.errors.address-lookup-failed");
    if (canFetchWeatherAfterLocationFailure(autoLocated)) {
      kLog.warn("{}; fetching weather using last resolved weather location", m_error);
      startWeatherFetch();
      return;
    }
    m_loading = false;
    scheduleRetryAfterFailure();
    dropPastForecastDays(m_snapshot);
    notifyChanged();
    return;
  }

  try {
    std::ifstream file(path);
    const auto json = nlohmann::json::parse(file);
    const double latitude = readNumber(json, "lat");
    const double longitude = readNumber(json, "lng");
    const std::string name = autoLocated ? readString(json, "city") : readString(json, "name");
    const std::string country = readString(json, "country");

    m_resolvedLatitude = latitude;
    m_resolvedLongitude = longitude;
    m_locationResolved = true;
    m_resolvedAddress = m_activeConfig.address;
    m_resolvedLocationName = compactLocationLabel(name, country);
    if (m_resolvedLocationName.empty()) {
      m_resolvedLocationName = autoLocated ? i18n::tr("weather.locations.current") : m_activeConfig.address;
    }

    kLog.info("weather location resolved");
    startWeatherFetch();
  } catch (const std::exception& e) {
    m_loading = false;
    m_error = autoLocated ? i18n::tr("weather.errors.parse-ip-geolocation") : i18n::tr("weather.errors.parse-geocode");
    if (canFetchWeatherAfterLocationFailure(autoLocated)) {
      kLog.warn("{}: {}; fetching weather using last resolved weather location", m_error, e.what());
      startWeatherFetch();
      return;
    }
    scheduleRetryAfterFailure();
    kLog.warn("{}: {}", m_error, e.what());
    notifyChanged();
  }
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
    dropPastForecastDays(m_snapshot);
    notifyChanged();
    return;
  }

  try {
    std::ifstream file(path);
    const auto json = nlohmann::json::parse(file);
    const auto& current = json.at("current_weather");
    const auto& daily = json.at("daily");
    const auto& dates = daily.at("time");
    const auto& tempsMax = daily.at("temperature_2m_max");
    const auto& tempsMin = daily.at("temperature_2m_min");
    const auto& codes = daily.at("weathercode");
    const auto& sunrises = daily.at("sunrise");
    const auto& sunsets = daily.at("sunset");

    WeatherSnapshot next;
    next.valid = true;
    next.locationName = m_resolvedLocationName;
    next.sourceLabel = m_activeConfig.autoLocate ? i18n::tr("weather.source.auto") : i18n::tr("weather.source.address");
    next.latitude = m_resolvedLatitude;
    next.longitude = m_resolvedLongitude;
    next.generationTimeMs = readOptionalNumber(json, "generationtime_ms");
    next.utcOffsetSeconds = readOptionalInt(json, "utc_offset_seconds");
    next.timezone = readString(json, "timezone");
    next.timezoneAbbreviation = readString(json, "timezone_abbreviation");
    next.elevationM = readOptionalNumber(json, "elevation");
    if (const auto it = json.find("current_weather_units"); it != json.end() && it->is_object()) {
      next.currentUnits.time = readString(*it, "time");
      next.currentUnits.interval = readString(*it, "interval");
      next.currentUnits.temperature = readString(*it, "temperature");
      next.currentUnits.windSpeed = readString(*it, "windspeed");
      next.currentUnits.windDirection = readString(*it, "winddirection");
      next.currentUnits.isDay = readString(*it, "is_day");
      next.currentUnits.weatherCode = readString(*it, "weathercode");
    }
    if (const auto it = json.find("daily_units"); it != json.end() && it->is_object()) {
      next.dailyUnits.time = readString(*it, "time");
      next.dailyUnits.temperatureMax = readString(*it, "temperature_2m_max");
      next.dailyUnits.temperatureMin = readString(*it, "temperature_2m_min");
      next.dailyUnits.weatherCode = readString(*it, "weathercode");
      next.dailyUnits.sunrise = readString(*it, "sunrise");
      next.dailyUnits.sunset = readString(*it, "sunset");
    }
    next.current.timeIso = readString(current, "time");
    next.current.intervalSeconds = readOptionalInt(current, "interval");
    next.current.temperatureC = readNumber(current, "temperature");
    next.current.windSpeedKmh = readOptionalNumber(current, "windspeed");
    next.current.windDirectionDeg = readOptionalInt(current, "winddirection");
    next.current.isDay = readBool(current, "is_day", true);
    next.current.weatherCode = readInt(current, "weathercode");
    next.fetchedAt = Clock::now();

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

bool WeatherService::hasResolvedLocation() const noexcept {
  return m_locationResolved && std::isfinite(m_resolvedLatitude) && std::isfinite(m_resolvedLongitude) &&
         m_resolvedLatitude >= -90.0 && m_resolvedLatitude <= 90.0 && m_resolvedLongitude >= -180.0 &&
         m_resolvedLongitude <= 180.0;
}

bool WeatherService::canFetchWeatherAfterLocationFailure(bool autoLocated) const noexcept {
  if (!hasResolvedLocation()) {
    return false;
  }
  if (autoLocated) {
    return true;
  }
  return m_resolvedAddress == m_activeConfig.address;
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

std::string WeatherService::compactLocationLabel(const std::string& name, const std::string& country) {
  if (!name.empty() && !country.empty()) {
    return std::format("{}, {}", name, country);
  }
  if (!name.empty()) {
    return name;
  }
  return country;
}

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
    const bool cachedAutoLocate = readBool(json, "auto_locate");
    const std::string cachedAddress = readString(json, "address");
    if (cachedAutoLocate != m_activeConfig.autoLocate) {
      return;
    }
    if (!cachedAutoLocate && cachedAddress != m_activeConfig.address) {
      return;
    }

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
    if (const auto it = snapshot.find("current"); it != snapshot.end() && it->is_object()) {
      m_snapshot.current.timeIso = readString(*it, "time_iso");
      m_snapshot.current.intervalSeconds = readOptionalInt(*it, "interval_seconds");
      m_snapshot.current.temperatureC = readOptionalNumber(*it, "temperature_c");
      m_snapshot.current.windSpeedKmh = readOptionalNumber(*it, "wind_speed_kmh");
      m_snapshot.current.windDirectionDeg = readOptionalInt(*it, "wind_direction_deg");
      m_snapshot.current.isDay = readBool(*it, "is_day", true);
      m_snapshot.current.weatherCode = readOptionalInt(*it, "weather_code");
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
    dropPastForecastDays(m_snapshot);

    m_resolvedLatitude = m_snapshot.latitude;
    m_resolvedLongitude = m_snapshot.longitude;
    m_resolvedLocationName = m_snapshot.locationName;
    m_resolvedAddress = cachedAddress;
    m_locationResolved = true;
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
      {"auto_locate", m_activeConfig.autoLocate},
      {"address", m_activeConfig.address},
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
           {"current",
            {
                {"time_iso", m_snapshot.current.timeIso},
                {"interval_seconds", m_snapshot.current.intervalSeconds},
                {"temperature_c", m_snapshot.current.temperatureC},
                {"wind_speed_kmh", m_snapshot.current.windSpeedKmh},
                {"wind_direction_deg", m_snapshot.current.windDirectionDeg},
                {"is_day", m_snapshot.current.isDay},
                {"weather_code", m_snapshot.current.weatherCode},
            }},
           {"forecast_days", nlohmann::json::array()},
           {"fetched_at", toUnixSeconds(m_snapshot.fetchedAt)},
       }}
  };

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
