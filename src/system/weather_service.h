#pragma once

#include "config/config_service.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class HttpClient;

struct WeatherCurrentUnits {
  std::string time;
  std::string interval;
  std::string temperature;
  std::string windSpeed;
  std::string windDirection;
  std::string isDay;
  std::string weatherCode;
};

struct WeatherDailyUnits {
  std::string time;
  std::string temperatureMax;
  std::string temperatureMin;
  std::string weatherCode;
  std::string sunrise;
  std::string sunset;
};

struct WeatherCurrentConditions {
  std::string timeIso;
  std::int32_t intervalSeconds = 0;
  double temperatureC = 0.0;
  double windSpeedKmh = 0.0;
  std::int32_t windDirectionDeg = 0;
  bool isDay = true;
  std::int32_t weatherCode = 0;
};

struct WeatherForecastDay {
  std::string dateIso;
  std::int32_t weatherCode = 0;
  double temperatureMaxC = 0.0;
  double temperatureMinC = 0.0;
  std::string sunriseIso;
  std::string sunsetIso;
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
  WeatherCurrentConditions current;
  std::vector<WeatherForecastDay> forecastDays;
  std::chrono::system_clock::time_point fetchedAt{};
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

  [[nodiscard]] int pollTimeoutMs() const;
  void tick();
  void requestRefresh(bool resetLocation = false);

  [[nodiscard]] bool enabled() const noexcept;
  [[nodiscard]] bool effectsEnabled() const noexcept { return m_activeConfig.effects; }
  [[nodiscard]] bool locationConfigured() const noexcept;
  [[nodiscard]] bool loading() const noexcept { return m_loading; }
  [[nodiscard]] bool hasData() const noexcept { return m_snapshot.valid; }
  [[nodiscard]] std::optional<WeatherCoordinates> resolvedCoordinates() const noexcept;
  [[nodiscard]] const std::string& error() const noexcept { return m_error; }
  [[nodiscard]] const WeatherSnapshot& snapshot() const noexcept { return m_snapshot; }
  [[nodiscard]] bool useImperialTemperature() const noexcept;
  [[nodiscard]] bool useImperialWindSpeed() const noexcept;
  [[nodiscard]] double displayTemperature(double celsius) const noexcept;
  [[nodiscard]] const char* displayTemperatureUnit() const noexcept;
  [[nodiscard]] double displayWindSpeed(double kmh) const noexcept;
  [[nodiscard]] const char* displayWindSpeedUnit() const noexcept;
  [[nodiscard]] double displayElevation(double meters) const noexcept;
  [[nodiscard]] const char* displayElevationUnit() const noexcept;

  [[nodiscard]] static std::string glyphForCode(std::int32_t code, bool isDay);
  [[nodiscard]] static std::string shortDescriptionForCode(std::int32_t code);
  [[nodiscard]] static std::string descriptionForCode(std::int32_t code);

private:
  enum class RequestKind : std::uint8_t {
    None = 0,
    Geolocate = 1,
    GeocodeAddress = 2,
    FetchWeather = 3,
  };

  void onConfigReload();
  void clearState();
  void notifyChanged();
  void startGeolocate();
  void startAddressGeocode();
  void startWeatherFetch();
  void handleLocationResponse(const std::filesystem::path& path, bool autoLocated, bool success, std::uint64_t serial);
  void handleWeatherResponse(const std::filesystem::path& path, bool success, std::uint64_t serial);
  void scheduleRetryAfterFailure();
  void loadCache();
  void saveCache() const;
  [[nodiscard]] bool hasResolvedLocation() const noexcept;
  [[nodiscard]] bool canFetchWeatherAfterLocationFailure(bool autoLocated) const noexcept;

  [[nodiscard]] static std::filesystem::path transportCacheDir();
  [[nodiscard]] static std::filesystem::path stateCacheFilePath();
  [[nodiscard]] static std::string formatCoordinate(double value);
  [[nodiscard]] static std::string compactLocationLabel(const std::string& name, const std::string& country);

  ConfigService& m_configService;
  HttpClient& m_httpClient;
  WeatherConfig m_activeConfig;
  std::vector<ChangeCallback> m_callbacks;
  WeatherSnapshot m_snapshot;
  std::chrono::system_clock::time_point m_nextRefreshAt{};
  bool m_loading = false;
  bool m_refreshQueued = false;
  bool m_locationResolved = false;
  std::string m_error;
  std::string m_resolvedAddress;
  std::string m_resolvedLocationName;
  double m_resolvedLatitude = 0.0;
  double m_resolvedLongitude = 0.0;
  RequestKind m_requestKind = RequestKind::None;
  std::uint64_t m_requestSerial = 0;
};
