#include "system/location_service.h"

#include "config/config_service.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "json.hpp"
#include "net/http_client.h"
#include "util/string_utils.h"

#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace {

  constexpr Logger kLog("location");

  using Clock = std::chrono::system_clock;

  // IP-derived position can change while the shell runs (travel/VPN); re-resolve periodically.
  constexpr auto kRefreshInterval = std::chrono::hours(6);
  constexpr auto kRetryInterval = std::chrono::minutes(5);
  // Address geocoding is stable once resolved; idle until the address changes.
  constexpr auto kIdleInterval = std::chrono::hours(24 * 30);

  double readNumber(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_number()) {
      throw std::runtime_error(std::string("missing numeric key: ") + key);
    }
    return it->get<double>();
  }

  std::string readString(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_string()) {
      return {};
    }
    return it->get<std::string>();
  }

  bool readBool(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_boolean()) {
      return false;
    }
    return it->get<bool>();
  }

} // namespace

LocationService::LocationService(ConfigService& configService, HttpClient& httpClient)
    : m_configService(configService), m_httpClient(httpClient) {}

void LocationService::initialize() {
  m_config = m_configService.config().location;
  m_configService.addReloadCallback([this]() { onConfigReload(); });
  loadCache();
  if (networkResolutionConfigured() && !m_resolved) {
    requestRefresh();
  }
}

void LocationService::addChangeCallback(ChangeCallback callback) { m_callbacks.push_back(std::move(callback)); }

bool LocationService::networkResolutionConfigured() const noexcept {
  return m_config.autoLocate || !m_config.address.empty();
}

bool LocationService::resolving() const noexcept { return networkResolutionConfigured() && !m_resolved; }

bool LocationService::hasResolvedLocation() const noexcept { return m_resolved && coordinatesValid(); }

std::optional<ResolvedLocation> LocationService::resolvedLocation() const noexcept {
  if (!hasResolvedLocation()) {
    return std::nullopt;
  }
  return ResolvedLocation{
      .latitude = m_latitude, .longitude = m_longitude, .name = m_name, .sourceLabel = m_sourceLabel
  };
}

int LocationService::pollTimeoutMs() const {
  if (m_requestKind != RequestKind::None) {
    return -1;
  }
  if (m_refreshQueued) {
    return 0;
  }
  if (!networkResolutionConfigured()) {
    return -1;
  }
  const auto now = Clock::now();
  if (m_nextRefreshAt <= now) {
    return 0;
  }
  return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(m_nextRefreshAt - now).count());
}

void LocationService::tick() {
  if (m_requestKind != RequestKind::None) {
    return;
  }
  if (!networkResolutionConfigured()) {
    m_refreshQueued = false;
    if (m_resolved || !m_error.empty()) {
      clearResolved();
      m_error.clear();
      notifyChanged();
    }
    return;
  }

  const auto now = Clock::now();
  if (!m_refreshQueued && now < m_nextRefreshAt) {
    return;
  }
  m_refreshQueued = false;

  if (m_config.autoLocate) {
    startGeolocate();
    return;
  }
  if (!m_resolved || m_resolvedAutoLocate || m_resolvedAddress != m_config.address) {
    startAddressGeocode();
    return;
  }
  m_nextRefreshAt = now + kIdleInterval;
}

void LocationService::onConfigReload() {
  const LocationConfig next = m_configService.config().location;
  if (next == m_config) {
    return;
  }
  const bool resolutionInputsChanged = next.autoLocate != m_config.autoLocate || next.address != m_config.address;
  m_config = next;
  if (resolutionInputsChanged) {
    m_error.clear();
    requestRefresh();
  }
}

void LocationService::notifyChanged() {
  for (const auto& callback : m_callbacks) {
    callback();
  }
}

void LocationService::requestRefresh() {
  m_refreshQueued = true;
  m_nextRefreshAt = Clock::time_point{};
}

void LocationService::startGeolocate() {
  std::error_code ec;
  std::filesystem::create_directories(transportCacheDir(), ec);
  const auto path = transportCacheDir() / "geolocate.json";
  const std::uint64_t serial = ++m_requestSerial;
  m_requestKind = RequestKind::Geolocate;
  m_httpClient.download("https://api.noctalia.dev/geolocate", path, [this, path, serial](bool success) {
    handleResponse(path, true, success, serial);
  });
}

void LocationService::startAddressGeocode() {
  std::error_code ec;
  std::filesystem::create_directories(transportCacheDir(), ec);
  const auto path = transportCacheDir() / "geocode.json";
  const std::string url = "https://api.noctalia.dev/geocode?city=" + StringUtils::urlEncode(m_config.address);
  const std::uint64_t serial = ++m_requestSerial;
  m_requestKind = RequestKind::GeocodeAddress;
  m_httpClient.download(url, path, [this, path, serial](bool success) {
    handleResponse(path, false, success, serial);
  });
}

void LocationService::handleResponse(
    const std::filesystem::path& path, bool autoLocated, bool success, std::uint64_t serial
) {
  if (serial != m_requestSerial) {
    return;
  }
  m_requestKind = RequestKind::None;

  if (!success) {
    m_error = autoLocated ? i18n::tr("location.errors.ip-geolocation-failed")
                          : i18n::tr("location.errors.address-lookup-failed");
    kLog.warn("{}", m_error);
    scheduleRetryAfterFailure();
    notifyChanged(); // serve last-known-good coordinates if we have them
    return;
  }

  try {
    std::ifstream file(path);
    const auto json = nlohmann::json::parse(file);
    const double latitude = readNumber(json, "lat");
    const double longitude = readNumber(json, "lng");
    const std::string name = autoLocated ? readString(json, "city") : readString(json, "name");
    const std::string country = readString(json, "country");

    m_latitude = latitude;
    m_longitude = longitude;
    m_resolved = true;
    m_resolvedAutoLocate = autoLocated;
    m_resolvedAddress = m_config.address;
    m_sourceLabel = autoLocated ? i18n::tr("location.source.auto") : i18n::tr("location.source.address");
    m_name = compactLocationLabel(name, country);
    if (m_name.empty()) {
      m_name = autoLocated ? i18n::tr("location.locations.current") : m_config.address;
    }
    m_error.clear();
    m_nextRefreshAt = Clock::now() + kRefreshInterval;
    kLog.info("location resolved");
    saveCache();
    notifyChanged();
  } catch (const std::exception& e) {
    m_error =
        autoLocated ? i18n::tr("location.errors.parse-ip-geolocation") : i18n::tr("location.errors.parse-geocode");
    kLog.warn("{}: {}", m_error, e.what());
    scheduleRetryAfterFailure();
    notifyChanged();
  }
}

void LocationService::scheduleRetryAfterFailure() {
  m_refreshQueued = false;
  m_nextRefreshAt = Clock::now() + kRetryInterval;
}

void LocationService::clearResolved() {
  ++m_requestSerial;
  m_requestKind = RequestKind::None;
  m_resolved = false;
  m_resolvedAutoLocate = false;
  m_resolvedAddress.clear();
  m_name.clear();
  m_sourceLabel.clear();
  m_latitude = 0.0;
  m_longitude = 0.0;
  m_nextRefreshAt = Clock::time_point{};
}

bool LocationService::coordinatesValid() const noexcept {
  return std::isfinite(m_latitude)
      && std::isfinite(m_longitude)
      && m_latitude >= -90.0
      && m_latitude <= 90.0
      && m_longitude >= -180.0
      && m_longitude <= 180.0;
}

std::filesystem::path LocationService::transportCacheDir() {
  return std::filesystem::path("/tmp") / "noctalia-location";
}

std::filesystem::path LocationService::stateCacheFilePath() {
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] != '\0') {
    return std::filesystem::path(xdg) / "noctalia" / "location.json";
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    return std::filesystem::path(home) / ".cache" / "noctalia" / "location.json";
  }
  return std::filesystem::path("/tmp") / "noctalia-location-cache.json";
}

std::string LocationService::compactLocationLabel(const std::string& name, const std::string& country) {
  if (!name.empty() && !country.empty()) {
    return std::format("{}, {}", name, country);
  }
  if (!name.empty()) {
    return name;
  }
  return country;
}

void LocationService::loadCache() {
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
    if (cachedAutoLocate != m_config.autoLocate) {
      return;
    }
    if (!cachedAutoLocate && cachedAddress != m_config.address) {
      return;
    }

    m_latitude = readNumber(json, "latitude");
    m_longitude = readNumber(json, "longitude");
    if (!coordinatesValid()) {
      m_latitude = 0.0;
      m_longitude = 0.0;
      return;
    }
    m_name = readString(json, "name");
    m_sourceLabel = readString(json, "source_label");
    m_resolvedAutoLocate = cachedAutoLocate;
    m_resolvedAddress = cachedAddress;
    m_resolved = true;
    m_nextRefreshAt = Clock::now() + kRefreshInterval;
    kLog.info("loaded cached location");
  } catch (const std::exception& e) {
    kLog.warn("failed to load location cache: {}", e.what());
  }
}

void LocationService::saveCache() const {
  if (!m_resolved) {
    return;
  }
  const auto path = stateCacheFilePath();
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);

  const nlohmann::json json{
      {"auto_locate", m_resolvedAutoLocate},
      {"address", m_resolvedAddress},
      {"latitude", m_latitude},
      {"longitude", m_longitude},
      {"name", m_name},
      {"source_label", m_sourceLabel},
  };
  try {
    std::ofstream file(path);
    file << json.dump(2);
  } catch (const std::exception& e) {
    kLog.warn("failed to save location cache: {}", e.what());
  }
}
