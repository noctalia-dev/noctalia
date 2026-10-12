#pragma once

#include "config/config_types.h"
#include "net/http_client.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

class ConfigService;

namespace scripting {
  class PluginManager;

  struct PluginMetric {
    std::uint64_t recommendations = 0;
    double trendingScore = 0;
  };

  enum class RecommendationError : std::uint8_t {
    None,
    Network,
    NotRecommendable,
    RateLimited,
    Unavailable,
    Protocol,
    State
  };

  struct RecommendationState {
    bool recommended = false;
    bool pending = false;
    bool mayBeCommitted = false;
    std::optional<bool> failedDesired;
    RecommendationError error = RecommendationError::None;
    std::chrono::steady_clock::time_point retryAt;
  };

  // Accountless discovery state, independent of plugin enablement and telemetry.
  class PluginRecommendations {
  public:
    PluginRecommendations(ConfigService& config, HttpClient& http, PluginManager& manager);
    ~PluginRecommendations();
    PluginRecommendations(const PluginRecommendations&) = delete;
    PluginRecommendations& operator=(const PluginRecommendations&) = delete;

    [[nodiscard]] static std::optional<std::string> pluginKey(const PluginSourceConfig& source, std::string_view id);
    [[nodiscard]] static std::optional<std::string> voterToken(std::string_view secret, std::string_view key);
    [[nodiscard]] static std::optional<std::unordered_map<std::string, PluginMetric>>
    parseMetrics(std::string_view body);
    [[nodiscard]] bool metricsAvailable() const;
    [[nodiscard]] std::optional<PluginMetric> metric(std::string_view key) const;
    [[nodiscard]] std::optional<std::uint64_t> count(std::string_view key) const;
    [[nodiscard]] RecommendationState state(std::string_view key);
    [[nodiscard]] std::vector<std::string> recommendedKeys();
    void fetchMetrics();
    void setRecommended(const PluginSourceConfig& source, std::string id, bool recommended);
    void setOnChanged(std::function<void()> cb) { m_onChanged = std::move(cb); }

  private:
    bool loadState();
    [[nodiscard]] std::optional<std::string> ensureToken(std::string_view key);
    void changed();

    ConfigService& m_config;
    HttpClient& m_http;
    PluginManager& m_manager;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::function<void()> m_onChanged;
    bool m_stateLoaded = false;
    bool m_stateValid = false;
    std::unordered_set<std::string> m_recommended;
    std::unordered_map<std::string, RecommendationState> m_states;
    std::unordered_map<std::string, PluginMetric> m_metrics;
    std::unordered_map<std::string, std::optional<std::uint64_t>> m_counts;
    std::string m_etag;
    bool m_hasDocument = false;
    bool m_metricsAvailable = false;
    bool m_fetching = false;
    std::uint64_t m_writeGeneration = 0;
    std::chrono::steady_clock::time_point m_freshUntil;
    std::chrono::steady_clock::time_point m_readRetryAt;
    std::chrono::steady_clock::time_point m_writeRetryAt;
  };
} // namespace scripting
