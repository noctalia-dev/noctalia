#include "scripting/plugin_recommendations.h"

#include "config/config_service.h"
#include "core/log.h"
#include "net/uri.h"
#include "scripting/plugin_id.h"
#include "scripting/plugin_manager.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_source_paths.h"
#include "security/secure_buffer.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <ctime>
#include <nlohmann/json.hpp>
#include <sodium.h>

namespace scripting {
  namespace {
    constexpr Logger kLog("plugin-recommendations");
    constexpr std::string_view kApi = "https://api.noctalia.dev";
    constexpr std::string_view kOwner = "plugin_store";
    using Clock = std::chrono::steady_clock;

    bool validKey(std::string_view key) {
      const auto colon = key.find(':');
      return colon != std::string_view::npos
          && (key.substr(0, colon) == "official" || key.substr(0, colon) == "community")
          && isValidPluginId(key.substr(colon + 1));
    }

    std::string encodeToken(std::span<const std::uint8_t> bytes) {
      std::array<char, 44> encoded{};
      sodium_bin2base64(
          encoded.data(), encoded.size(), bytes.data(), bytes.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING
      );
      return encoded.data();
    }

    std::optional<std::uint64_t> parseCount(const nlohmann::json& value) {
      if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
      }
      if (value.is_number_integer() && value.get<std::int64_t>() >= 0) {
        return static_cast<std::uint64_t>(value.get<std::int64_t>());
      }
      return std::nullopt;
    }

    std::chrono::seconds cacheLifetime(const HttpResponse& response) {
      const auto header = response.headers.find("cache-control");
      if (header == response.headers.end()) {
        return std::chrono::seconds(0);
      }
      const std::string_view value = header->second;
      const auto pos = value.find("max-age=");
      if (pos == std::string_view::npos) {
        return std::chrono::seconds(0);
      }
      const auto first = value.data() + pos + std::string_view("max-age=").size();
      std::int64_t seconds = 0;
      if (std::from_chars(first, value.data() + value.size(), seconds).ec != std::errc{} || seconds < 0) {
        return std::chrono::seconds(0);
      }
      return std::chrono::seconds(std::min<std::int64_t>(seconds, 60));
    }

    std::chrono::seconds retryDelay(const HttpResponse& response) {
      // Retry-After accepts delta seconds or an HTTP date. No automatic retry is scheduled.
      if (const auto it = response.headers.find("retry-after"); it != response.headers.end()) {
        std::int64_t seconds = 0;
        const auto [end, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), seconds);
        if (ec == std::errc{} && end == it->second.data() + it->second.size() && seconds >= 0) {
          return std::chrono::seconds(std::min<std::int64_t>(seconds, 86400));
        }
        const auto date = curl_getdate(it->second.c_str(), nullptr);
        if (date >= 0) {
          return std::chrono::seconds(std::clamp<std::int64_t>(date - std::time(nullptr), 0, 86400));
        }
      }
      return std::chrono::seconds(60);
    }
  } // namespace

  PluginRecommendations::PluginRecommendations(ConfigService& config, HttpClient& http, PluginManager& manager)
      : m_config(config), m_http(http), m_manager(manager) {}

  PluginRecommendations::~PluginRecommendations() { *m_alive = false; }

  std::optional<std::string> PluginRecommendations::pluginKey(const PluginSourceConfig& source, std::string_view id) {
    if (!source.enabled || source.kind != PluginSourceKind::Git || !isValidPluginId(id)) {
      return std::nullopt;
    }
    for (const auto& builtin : defaultPluginSources()) {
      if (source.name == builtin.name && source.location == builtin.location) {
        return source.name + ":" + std::string(id);
      }
    }
    return std::nullopt;
  }

  std::optional<std::string> PluginRecommendations::voterToken(std::string_view secret, std::string_view key) {
    if (!validKey(key) || secret.size() != 43 || !security::initializeSecurityPrimitives()) {
      return std::nullopt;
    }
    security::SecureBuffer bytes(32);
    std::size_t length = 0;
    if (sodium_base642bin(
            bytes.bytes().data(), bytes.size(), secret.data(), secret.size(), nullptr, &length, nullptr,
            sodium_base64_VARIANT_URLSAFE_NO_PADDING
        ) != 0
        || length != bytes.size()
        || encodeToken(bytes.bytes()) != secret) {
      return std::nullopt;
    }
    constexpr char prefix[] = "noctalia-recommend-v1\0";
    std::string message(prefix, sizeof(prefix) - 1);
    message += key;
    std::array<std::uint8_t, crypto_auth_hmacsha256_BYTES> digest{};
    crypto_auth_hmacsha256(
        digest.data(), reinterpret_cast<const unsigned char*>(message.data()), message.size(), bytes.bytes().data()
    );
    return encodeToken(digest);
  }

  std::optional<std::unordered_map<std::string, PluginMetric>>
  PluginRecommendations::parseMetrics(std::string_view body) {
    const auto doc = nlohmann::json::parse(body, nullptr, false);
    if (!doc.is_object()
        || !doc.contains("schema")
        || !doc["schema"].is_number_integer()
        || doc["schema"] != 1
        || !doc.contains("generated_at")
        || !doc["generated_at"].is_string()
        || !doc.contains("plugins")
        || !doc["plugins"].is_array()) {
      return std::nullopt;
    }
    std::unordered_map<std::string, PluginMetric> metrics;
    for (const auto& item : doc["plugins"]) {
      if (!item.is_object()
          || !item.contains("key")
          || !item["key"].is_string()
          || !item.contains("recommendations")
          || !item.contains("trending_score")
          || !item["trending_score"].is_number()) {
        return std::nullopt;
      }
      const auto key = item["key"].get<std::string>();
      const auto count = parseCount(item["recommendations"]);
      const auto trending = item["trending_score"].get<double>();
      if (!validKey(key)
          || !count.has_value()
          || !std::isfinite(trending)
          || trending < 0
          || !metrics.emplace(key, PluginMetric{*count, trending}).second) {
        return std::nullopt;
      }
    }
    return metrics;
  }

  bool PluginRecommendations::metricsAvailable() const {
    return m_metricsAvailable && !m_config.config().shell.offlineMode;
  }

  std::optional<PluginMetric> PluginRecommendations::metric(std::string_view key) const {
    if (metricsAvailable()) {
      if (const auto it = m_metrics.find(std::string(key)); it != m_metrics.end()) {
        return it->second;
      }
    }
    return std::nullopt;
  }

  std::optional<std::uint64_t> PluginRecommendations::count(std::string_view key) const {
    if (!m_config.config().shell.offlineMode) {
      if (const auto it = m_counts.find(std::string(key)); it != m_counts.end()) {
        return it->second;
      }
      if (const auto value = metric(key)) {
        return value->recommendations;
      }
    }
    return std::nullopt;
  }

  bool PluginRecommendations::loadState() {
    if (m_stateLoaded) {
      return m_stateValid;
    }
    m_stateLoaded = true;
    if (!m_config.stateParseError().empty() || !m_config.stateOwnerValid(kOwner)) {
      kLog.warn("invalid recommendation state");
      return false;
    }
    const auto keys = m_config.stateStringArray(kOwner, "recommended_plugin_keys");
    if (!keys.has_value() && m_config.stateContains(kOwner, "recommended_plugin_keys")) {
      return false;
    }
    if (keys.has_value()) {
      for (const auto& key : *keys) {
        if (!validKey(key)) {
          kLog.warn("invalid recommendation key in local state");
          return false;
        }
        m_recommended.insert(key);
      }
    }
    m_stateValid = true;
    return true;
  }

  std::vector<std::string> PluginRecommendations::recommendedKeys() {
    if (!loadState()) {
      return {};
    }
    std::vector<std::string> keys(m_recommended.begin(), m_recommended.end());
    std::ranges::sort(keys);
    return keys;
  }

  RecommendationState PluginRecommendations::state(std::string_view key) {
    RecommendationState result;
    if (!loadState()) {
      result.error = RecommendationError::State;
      return result;
    }
    if (const auto it = m_states.find(std::string(key)); it != m_states.end()) {
      result = it->second;
    }
    result.recommended = m_recommended.contains(std::string(key));
    return result;
  }

  std::optional<std::string> PluginRecommendations::ensureToken(std::string_view key) {
    auto secret = m_config.stateString(kOwner, "recommendation_secret");
    if (!secret.has_value()) {
      if (m_config.stateContains(kOwner, "recommendation_secret") || !m_recommended.empty()) {
        return std::nullopt;
      }
      if (!security::initializeSecurityPrimitives()) {
        return std::nullopt;
      }
      auto random = security::SecureKey::generate();
      if (!random.has_value()) {
        return std::nullopt;
      }
      secret = encodeToken(random->bytes());
      if (!m_config.setStateString(kOwner, "recommendation_secret", *secret)) {
        return std::nullopt;
      }
    }
    return voterToken(*secret, key);
  }

  void PluginRecommendations::changed() {
    if (m_onChanged) {
      m_onChanged();
    }
  }

  void PluginRecommendations::fetchMetrics() {
    if (m_config.config().shell.offlineMode || m_fetching || Clock::now() < m_readRetryAt) {
      return;
    }
    if (m_hasDocument && Clock::now() < m_freshUntil) {
      m_metricsAvailable = true;
      changed();
      return;
    }
    m_fetching = true;
    HttpRequest request;
    request.url = std::string(kApi) + "/v1/plugin-metrics";
    if (!m_etag.empty()) {
      request.headers.push_back("If-None-Match: " + m_etag);
    }
    const auto generation = m_writeGeneration;
    m_http.request(std::move(request), [this, alive = m_alive, generation](HttpResponse response) {
      if (!*alive) {
        return;
      }
      m_fetching = false;
      if (generation != m_writeGeneration) {
        fetchMetrics();
        return;
      }
      bool ok = response.transportOk && response.status == 304 && m_hasDocument;
      if (response.transportOk && response.status == 200) {
        if (auto metrics = parseMetrics(response.body)) {
          m_metrics = std::move(*metrics);
          m_hasDocument = true;
          ok = true;
        } else {
          kLog.warn("unsupported or invalid plugin metrics document");
        }
      }
      m_metricsAvailable = ok;
      if (ok) {
        m_counts.clear();
        const auto etag = response.headers.find("etag");
        m_etag = etag == response.headers.end() ? std::string() : etag->second;
        m_freshUntil = Clock::now() + cacheLifetime(response);
      } else {
        m_counts.clear();
        m_freshUntil = {};
        m_readRetryAt = Clock::now() + (response.status == 429 ? retryDelay(response) : std::chrono::seconds(60));
        kLog.warn("plugin metrics unavailable (HTTP {})", response.status);
      }
      changed();
    });
  }

  void PluginRecommendations::setRecommended(const PluginSourceConfig& source, std::string id, bool recommended) {
    const auto key = pluginKey(source, id);
    if (!key.has_value() || m_config.config().shell.offlineMode || !loadState()) {
      return;
    }
    auto& status = m_states[*key];
    if (status.pending || Clock::now() < std::max(status.retryAt, m_writeRetryAt)) {
      return;
    }
    const auto& sources = m_config.config().plugins.sources;
    const bool currentSource = std::ranges::any_of(sources, [&](const auto& current) { return current == source; });
    const auto plugins = m_manager.list(CatalogAccess::LocalOnly);
    const auto plugin = std::ranges::find(plugins, id, &PluginStatus::id);
    const bool owned = plugin == plugins.end() || plugin->source == source.name;
    bool installed = false;
    if (plugin != plugins.end()
        && plugin->source == source.name
        && plugin->materialized
        && !plugin->deprecated
        && !m_manager.isEnabling(id)) {
      const auto subdir = pluginSubdirFromId(id);
      const auto manifest =
          parsePluginManifest(plugin_paths::gitMaterializedRoot(source) / *subdir / "plugin.toml", nullptr);
      installed = manifest.has_value() && manifest->id == id;
    }
    const bool retryingTrue = m_recommended.contains(*key) || (status.failedDesired == true && status.mayBeCommitted);
    if (!currentSource || !owned || (recommended && !installed && !retryingTrue)) {
      status.error = RecommendationError::NotRecommendable;
      changed();
      return;
    }
    const auto token = ensureToken(*key);
    if (!token.has_value()) {
      status.error = RecommendationError::State;
      changed();
      return;
    }
    status.pending = true;
    status.error = RecommendationError::None;
    status.failedDesired.reset();
    HttpRequest request;
    request.method = "PUT";
    const auto slash = id.find('/');
    request.url = std::string(kApi)
        + "/v1/plugins/"
        + uri::encodeComponent(source.name)
        + "/"
        + uri::encodeComponent(std::string_view(id).substr(0, slash))
        + "/"
        + uri::encodeComponent(std::string_view(id).substr(slash + 1))
        + "/recommendation";
    request.headers = {"Content-Type: application/json"};
    request.body = nlohmann::json{{"voter_token", *token}, {"recommended", recommended}}.dump();
    m_http.request(std::move(request), [this, alive = m_alive, key = *key, recommended](HttpResponse response) {
      if (!*alive) {
        return;
      }
      auto& current = m_states[key];
      current.pending = false;
      current.mayBeCommitted =
          !response.transportOk || response.status == 200 || (response.status >= 500 && response.status != 503);
      current.failedDesired = recommended;
      current.error = RecommendationError::Network;
      if (response.transportOk && response.status == 200) {
        const auto doc = nlohmann::json::parse(response.body, nullptr, false);
        const bool valid = doc.is_object()
            && doc.contains("recommended")
            && doc["recommended"].is_boolean()
            && doc["recommended"].get<bool>() == recommended
            && doc.contains("recommendations")
            && (doc["recommendations"].is_null() || parseCount(doc["recommendations"]).has_value());
        if (valid) {
          auto keys = m_recommended;
          if (recommended) {
            keys.insert(key);
          } else {
            keys.erase(key);
          }
          std::vector<std::string> saved(keys.begin(), keys.end());
          std::ranges::sort(saved);
          if (m_config.setStateStringArray(kOwner, "recommended_plugin_keys", saved)) {
            m_recommended = std::move(keys);
            current.failedDesired.reset();
            current.mayBeCommitted = false;
            current.error = RecommendationError::None;
          } else {
            current.error = RecommendationError::State;
          }
          m_counts[key] = parseCount(doc["recommendations"]);
          if (doc["recommendations"].is_null()) {
            m_metrics.erase(key);
          } else if (auto it = m_metrics.find(key); it != m_metrics.end()) {
            it->second.recommendations = *m_counts[key];
          }
          ++m_writeGeneration;
          m_freshUntil = {};
          m_readRetryAt = {};
          changed();
          fetchMetrics();
          return;
        }
        current.error = RecommendationError::Protocol;
      } else if (response.transportOk) {
        switch (response.status) {
        case 404:
        case 409:
          current.error = RecommendationError::NotRecommendable;
          break;
        case 429:
          current.error = RecommendationError::RateLimited;
          current.retryAt = Clock::now() + retryDelay(response);
          m_writeRetryAt = current.retryAt;
          break;
        case 503:
          current.error = RecommendationError::Unavailable;
          break;
        case 408:
          current.error = RecommendationError::Network;
          break;
        default:
          current.error = response.status >= 500 ? RecommendationError::Network : RecommendationError::Protocol;
          break;
        }
      }
      if (current.error == RecommendationError::Network || current.error == RecommendationError::Unavailable) {
        current.retryAt = Clock::now() + std::chrono::seconds(5);
      }
      kLog.warn("recommendation request not confirmed (HTTP {})", response.status);
      changed();
    });
    changed();
  }
} // namespace scripting
