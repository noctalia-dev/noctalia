#pragma once

#include "config/config_types.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class ConfigService;
class HttpClient;

struct WeatherCurrentUnits {
  std::string time;
  std::string interval;
  std::string temperature;
  std::string windSpeed;
  std::string windDirection;
  std::string isDay;
  std::string weatherCode;
  std::string uvIndex;
};

struct WeatherDailyUnits {
  std::string time;
  std::string temperatureMax;
  std::string temperatureMin;
  std::string weatherCode;
  std::string sunrise;
  std::string sunset;
};

struct WeatherHourlyUnits {
  std::string time;
  std::string temperature;
  std::string relativeHumidity;
  std::string precipitationProbability;
  std::string weatherCode;
  std::string isDay;
  std::string windSpeed;
};

struct WeatherCurrentConditions {
  std::string timeIso;
  std::int32_t intervalSeconds = 0;
  double temperatureC = 0.0;
  double windSpeedKmh = 0.0;
  std::int32_t windDirectionDeg = 0;
  bool isDay = true;
  std::int32_t weatherCode = 0;
  double uvIndex = 0.0;
};

struct WeatherForecastDay {
  std::string dateIso;
  std::int32_t weatherCode = 0;
  double temperatureMaxC = 0.0;
  double temperatureMinC = 0.0;
  std::string sunriseIso;
  std::string sunsetIso;
};

struct WeatherForecastHour {
  std::string timeIso;
  std::int32_t weatherCode = 0;
  double temperatureC = 0.0;
  std::int32_t relativeHumidityPercent = 0;
  std::int32_t precipitationProbabilityPercent = 0;
  bool isDay = true;
  double windSpeedKmh = 0.0;
};

struct WeatherSnapshot {
  bool valid = false;
  std::string locationName;
  std::string sourceLabel;
  double latitude = 0.0;
  double longitude = 0.0;
  double generationTimeMs = 0.0;
  std::int32_t utcOffsetSeconds = 0;
  std::string timezone;
  std::string timezoneAbbreviation;
  double elevationM = 0.0;
  WeatherCurrentUnits currentUnits;
  WeatherDailyUnits dailyUnits;
  WeatherHourlyUnits hourlyUnits;
  WeatherCurrentConditions current;
  std::vector<WeatherForecastHour> forecastHours;
  std::vector<WeatherForecastDay> forecastDays;
  std::chrono::system_clock::time_point fetchedAt;
};

struct WeatherCoordinates {
  double latitude = 0.0;
  double longitude = 0.0;
};

class WeatherService {
public:
  using ChangeCallback = std::function<void()>;

  WeatherService(ConfigService& configService, HttpClient& httpClient);

  void initialize();
  void addChangeCallback(ChangeCallback callback);

  // Coordinates published by LocationService (IP geolocation or geocoded address) plus a
  // display name and source label. Passing std::nullopt means no location is available.
  void setLocation(std::optional<WeatherCoordinates> coordinates, std::string name, std::string sourceLabel);

  [[nodiscard]] int pollTimeoutMs() const;
  void tick();
  void requestRefresh();

  [[nodiscard]] bool enabled() const noexcept;
  [[nodiscard]] bool effectsEnabled() const noexcept { return m_activeConfig.effects; }
  [[nodiscard]] bool locationConfigured() const noexcept;
  [[nodiscard]] bool loading() const noexcept { return m_loading; }
  [[nodiscard]] bool hasData() const noexcept { return m_snapshot.valid; }
  [[nodiscard]] const std::string& error() const noexcept { return m_error; }
  [[nodiscard]] const WeatherSnapshot& snapshot() const noexcept { return m_snapshot; }
  [[nodiscard]] bool useImperial() const noexcept;
  [[nodiscard]] double displayTemperature(double celsius) const noexcept;
  [[nodiscard]] const char* displayTemperatureUnit() const noexcept;

  [[nodiscard]] static std::string glyphForCode(std::int32_t code, bool isDay);
  [[nodiscard]] static std::string shortDescriptionForCode(std::int32_t code);
  [[nodiscard]] static std::string descriptionForCode(std::int32_t code);

private:
  enum class RequestKind : std::uint8_t {
    None = 0,
    FetchWeather = 1,
  };

  void onConfigReload();
  void clearState();
  void notifyChanged();
  void startWeatherFetch();
  void handleWeatherResponse(const std::filesystem::path& path, bool success, std::uint64_t serial);
  void scheduleRetryAfterFailure();
  void loadCache();
  void saveCache() const;
  [[nodiscard]] bool coordinatesValid() const noexcept;

  [[nodiscard]] static std::filesystem::path transportCacheDir();
  [[nodiscard]] static std::filesystem::path stateCacheFilePath();
  [[nodiscard]] static std::string formatCoordinate(double value);

  ConfigService& m_configService;
  HttpClient& m_httpClient;
  WeatherConfig m_activeConfig;
  std::vector<ChangeCallback> m_callbacks;
  WeatherSnapshot m_snapshot;
  std::chrono::system_clock::time_point m_nextRefreshAt;
  bool m_loading = false;
  bool m_refreshQueued = false;
  bool m_hasLocation = false;
  std::string m_error;
  std::string m_locationName;
  std::string m_locationSource;
  double m_resolvedLatitude = 0.0;
  double m_resolvedLongitude = 0.0;
  RequestKind m_requestKind = RequestKind::None;
  std::uint64_t m_requestSerial = 0;
};
