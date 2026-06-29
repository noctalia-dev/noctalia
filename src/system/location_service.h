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

// Coordinates resolved from IP geolocation or a geocoded address.
struct ResolvedLocation {
  double latitude = 0.0;
  double longitude = 0.0;
  std::string name;        // display label, e.g. "Berlin, Germany"
  std::string sourceLabel; // i18n label describing how the location was resolved
};

// Owns "where am I" for the whole shell. Resolves coordinates from IP geolocation
// (auto_locate) or a geocoded address and publishes them to consumers (weather,
// night light, theme auto mode). Manual latitude/longitude and fixed sunrise/sunset
// live in LocationConfig and are read directly by those consumers; this service only
// performs the network resolution. Runs independently of whether weather is enabled.
class LocationService {
public:
  using ChangeCallback = std::function<void()>;

  LocationService(ConfigService& configService, HttpClient& httpClient);

  void initialize();
  void addChangeCallback(ChangeCallback callback);

  [[nodiscard]] int pollTimeoutMs() const;
  void tick();

  // auto_locate is on, or an address is set: a network resolution is requested.
  [[nodiscard]] bool networkResolutionConfigured() const noexcept;
  // Network resolution is requested but coordinates are not yet available.
  [[nodiscard]] bool resolving() const noexcept;
  [[nodiscard]] bool hasResolvedLocation() const noexcept;
  [[nodiscard]] std::optional<ResolvedLocation> resolvedLocation() const noexcept;
  [[nodiscard]] const std::string& error() const noexcept { return m_error; }

private:
  enum class RequestKind : std::uint8_t {
    None = 0,
    Geolocate = 1,
    GeocodeAddress = 2,
  };

  void onConfigReload();
  void notifyChanged();
  void requestRefresh();
  void startGeolocate();
  void startAddressGeocode();
  void handleResponse(const std::filesystem::path& path, bool autoLocated, bool success, std::uint64_t serial);
  void clearResolved();
  void scheduleRetryAfterFailure();
  void loadCache();
  void saveCache() const;
  [[nodiscard]] bool coordinatesValid() const noexcept;

  [[nodiscard]] static std::filesystem::path transportCacheDir();
  [[nodiscard]] static std::filesystem::path stateCacheFilePath();
  [[nodiscard]] static std::string compactLocationLabel(const std::string& name, const std::string& country);

  ConfigService& m_configService;
  HttpClient& m_httpClient;
  LocationConfig m_config;
  std::vector<ChangeCallback> m_callbacks;

  bool m_resolved = false;
  double m_latitude = 0.0;
  double m_longitude = 0.0;
  std::string m_name;
  std::string m_sourceLabel;
  std::string m_resolvedAddress;
  bool m_resolvedAutoLocate = false;
  std::string m_error;

  RequestKind m_requestKind = RequestKind::None;
  std::uint64_t m_requestSerial = 0;
  bool m_refreshQueued = false;
  std::chrono::system_clock::time_point m_nextRefreshAt;
};
