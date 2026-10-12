#include "config/config_service.h"
#include "config/state_store.h"
#include "core/deferred_call.h"
#include "scripting/plugin_api.h"
#include "scripting/plugin_manager.h"
#include "scripting/plugin_recommendations.h"
#include "scripting/plugin_source_paths.h"
#include "test_check.h"
#include "util/file_utils.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <print>
#include <string>

namespace {
  using scripting::PluginRecommendations;
  using scripting::RecommendationError;
  using Clock = std::chrono::steady_clock;

  void pump(HttpClient& http, const std::function<bool()>& done) {
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    do {
      for (auto& callback : DeferredCall::takePending()) {
        callback();
      }
      std::vector<pollfd> fds;
      http.addPollFds(fds);
      (void)::poll(fds.data(), fds.size(), 10);
      http.dispatch(fds, 0);
      TEST_CHECK(Clock::now() < deadline);
    } while (!done());
  }

  void protocolTests() {
    auto sources = defaultPluginSources();
    const std::string id = "CaseAuthor/Case.Plugin";
    const auto key = PluginRecommendations::pluginKey(sources[0], id);
    TEST_CHECK(key == "official:CaseAuthor/Case.Plugin");
    TEST_CHECK(PluginRecommendations::pluginKey(sources[1], id) == "community:CaseAuthor/Case.Plugin");
    sources[0].location += ".git";
    TEST_CHECK(!PluginRecommendations::pluginKey(sources[0], id).has_value());
    sources[1].name = "custom";
    TEST_CHECK(!PluginRecommendations::pluginKey(sources[1], id).has_value());
    TEST_CHECK(!PluginRecommendations::pluginKey(defaultPluginSources()[0], "author/plugin/extra").has_value());

    // Independent Python hmac/SHA256 vector: bytes 0..31, exact NUL-delimited domain and case-preserved key.
    constexpr std::string_view secret = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8";
    TEST_CHECK(PluginRecommendations::voterToken(secret, *key) == "3O8AtAsynuUo7PrrWG-amxnGAZ34sRgI533XftCNGVU");
    TEST_CHECK(
        PluginRecommendations::voterToken(secret, *key)
        != PluginRecommendations::voterToken(secret, "community:CaseAuthor/Case.Plugin")
    );
    TEST_CHECK(!PluginRecommendations::voterToken(std::string(secret) + "=", *key).has_value());
    TEST_CHECK(!PluginRecommendations::voterToken("invalid", *key).has_value());
    TEST_CHECK(!PluginRecommendations::voterToken(secret, "custom:author/plugin").has_value());

    const std::string doc =
        R"({"schema":1,"generated_at":"2026-10-09T12:00:00Z","plugins":[{"key":"official:CaseAuthor/Case.Plugin","recommendations":0,"trending_score":1.23456789}]})";
    const auto metrics = PluginRecommendations::parseMetrics(doc);
    TEST_CHECK(metrics.has_value());
    TEST_CHECK(metrics->at(*key).recommendations == 0);
    TEST_CHECK(metrics->at(*key).trendingScore == 1.23456789);
    TEST_CHECK(!metrics->contains("community:CaseAuthor/Case.Plugin"));
    auto invalid = nlohmann::json::parse(doc);
    invalid["schema"] = 2;
    TEST_CHECK(!PluginRecommendations::parseMetrics(invalid.dump()).has_value());
    invalid["schema"] = 1;
    for (const auto& value : {nlohmann::json(nullptr), nlohmann::json(-1), nlohmann::json(1.5), nlohmann::json("1")}) {
      invalid["plugins"][0]["recommendations"] = value;
      TEST_CHECK(!PluginRecommendations::parseMetrics(invalid.dump()).has_value());
    }
    invalid = nlohmann::json::parse(doc);
    invalid["plugins"].push_back(invalid["plugins"][0]);
    TEST_CHECK(!PluginRecommendations::parseMetrics(invalid.dump()).has_value());
    TEST_CHECK(!PluginRecommendations::parseMetrics("<html>proxy failure</html>").has_value());
  }

  void httpTests(std::filesystem::path caBundlePath) {
    const char* fixture = std::getenv("NOCTALIA_RECOMMENDATION_FIXTURE");
    TEST_CHECK(fixture != nullptr && std::string_view(fixture) == "1");
    const char* proxy = std::getenv("HTTPS_PROXY");
    TEST_CHECK(proxy != nullptr && std::string_view(proxy).starts_with("http://127.0.0.1:"));
    ConfigService config;
    TEST_CHECK(!caBundlePath.empty() && std::filesystem::is_regular_file(caBundlePath));
    HttpClient http(std::move(caBundlePath));
    scripting::PluginManager manager(config);
    const auto sources = defaultPluginSources();
    const auto& source = sources[0];
    const std::string id = "Fixture/CasePlugin";
    const std::string key = "official:" + id;
    PluginRecommendations service(config, http, manager);
    int updates = 0;
    service.setOnChanged([&] { ++updates; });
    service.fetchMetrics();
    pump(http, [&] { return service.metricsAvailable(); });
    const int initialUpdates = updates;
    service.fetchMetrics();
    pump(http, [&] { return updates > initialUpdates; });
    TEST_CHECK(service.metricsAvailable());
    TEST_CHECK(service.count(key) == 0);
    TEST_CHECK(!service.count("official:Fixture/Hidden").has_value());
    TEST_CHECK(!service.count("official:Fixture/Omitted").has_value());
    TEST_CHECK(!config.stateContains("plugin_store", "recommendation_secret"));

    service.setRecommended(source, "Fixture/Uninstalled", true);
    TEST_CHECK(service.state("official:Fixture/Uninstalled").error == RecommendationError::NotRecommendable);
    service.setRecommended(source, "Fixture/Deprecated", true);
    TEST_CHECK(service.state("official:Fixture/Deprecated").error == RecommendationError::NotRecommendable);

    service.setRecommended(source, id, true);
    TEST_CHECK(service.state(key).pending);
    TEST_CHECK(!service.state(key).recommended);
    service.setRecommended(source, id, true);
    service.setRecommended(source, id, false);
    TEST_CHECK(service.state(key).pending);
    pump(http, [&] { return !service.state(key).pending && service.count(key) == 1; });
    TEST_CHECK(service.state(key).recommended);
    const auto secret = config.stateString("plugin_store", "recommendation_secret");
    TEST_CHECK(secret.has_value() && secret->size() == 43);
    service.setRecommended(source, id, true);
    pump(http, [&] { return !service.state(key).pending; });
    TEST_CHECK(service.count(key) == 1);
    service.setRecommended(source, id, false);
    pump(http, [&] { return !service.state(key).pending; });
    TEST_CHECK(!service.state(key).recommended);
    TEST_CHECK(service.count(key) == 0);
    service.setRecommended(source, id, false);
    pump(http, [&] { return !service.state(key).pending; });
    TEST_CHECK(!service.state(key).recommended);
    TEST_CHECK(service.count(key) == 0);

    service.setRecommended(source, id, true);
    pump(http, [&] { return !service.state(key).pending; });
    std::filesystem::remove_all(scripting::plugin_paths::gitMaterializedRoot(source) / "CasePlugin");
    service.setRecommended(source, id, false);
    pump(http, [&] { return !service.state(key).pending; });
    TEST_CHECK(!service.state(key).recommended);
    TEST_CHECK(service.count(key) == 0);
    TEST_CHECK(config.stateString("plugin_store", "recommendation_secret") == secret);

    service.setRecommended(source, "Fixture/Delisted", true);
    pump(http, [&] { return !service.state("official:Fixture/Delisted").pending; });
    TEST_CHECK(service.state("official:Fixture/Delisted").recommended);
    TEST_CHECK(std::ranges::contains(service.recommendedKeys(), "official:Fixture/Delisted"));
    TEST_CHECK(!std::ranges::contains(
        manager.list(scripting::CatalogAccess::LocalOnly), "Fixture/Delisted", &scripting::PluginStatus::id
    ));
    service.setRecommended(source, "Fixture/Delisted", false);
    pump(http, [&] { return !service.state("official:Fixture/Delisted").pending; });
    TEST_CHECK(!service.state("official:Fixture/Delisted").recommended);
    TEST_CHECK(!std::ranges::contains(service.recommendedKeys(), "official:Fixture/Delisted"));
    TEST_CHECK(!config.buildSupportReport().contains(*secret));

    service.setRecommended(source, "Fixture/Hidden", true);
    pump(http, [&] { return !service.state("official:Fixture/Hidden").pending; });
    TEST_CHECK(service.state("official:Fixture/Hidden").recommended);
    TEST_CHECK(!service.count("official:Fixture/Hidden").has_value());

    for (const auto& [plugin, expected] :
         {std::pair{"Conflict", RecommendationError::NotRecommendable},
          std::pair{"Unavailable", RecommendationError::Unavailable}, std::pair{"Proxy", RecommendationError::Network},
          std::pair{"Limited", RecommendationError::RateLimited}}) {
      const auto pluginId = "Fixture/" + std::string(plugin);
      const auto pluginKey = "official:" + pluginId;
      service.setRecommended(source, pluginId, true);
      pump(http, [&] { return !service.state(pluginKey).pending; });
      TEST_CHECK(service.state(pluginKey).error == expected);
      TEST_CHECK(!service.state(pluginKey).recommended);
      TEST_CHECK(service.state(pluginKey).failedDesired == true);
    }
    std::filesystem::remove_all(scripting::plugin_paths::gitMaterializedRoot(source) / "Conflict");
    service.setRecommended(source, "Fixture/Conflict", true);
    TEST_CHECK(!service.state("official:Fixture/Conflict").pending);
    TEST_CHECK(service.state("official:Fixture/Conflict").error == RecommendationError::NotRecommendable);
    const auto limited = service.state("official:Fixture/Limited");
    service.setRecommended(source, "Fixture/Limited", true);
    TEST_CHECK(!service.state("official:Fixture/Limited").pending);
    pump(http, [&] { return Clock::now() >= limited.retryAt; });

    service.setRecommended(source, "Fixture/Lost", true);
    pump(http, [&] { return !service.state("official:Fixture/Lost").pending; });
    auto lost = service.state("official:Fixture/Lost");
    TEST_CHECK(!lost.recommended && lost.failedDesired == true && lost.mayBeCommitted);
    std::filesystem::remove_all(scripting::plugin_paths::gitMaterializedRoot(source) / "Lost");
    pump(http, [&] { return Clock::now() >= lost.retryAt; });
    service.setRecommended(source, "Fixture/Lost", true);
    pump(http, [&] { return !service.state("official:Fixture/Lost").pending; });
    TEST_CHECK(service.state("official:Fixture/Lost").recommended);
    TEST_CHECK(service.count("official:Fixture/Lost") == 1);

    ConfigService reloadedConfig;
    scripting::PluginManager reloadedManager(reloadedConfig);
    PluginRecommendations reloaded(reloadedConfig, http, reloadedManager);
    TEST_CHECK(reloaded.state("official:Fixture/Lost").recommended);
    TEST_CHECK(reloadedConfig.stateString("plugin_store", "recommendation_secret") == secret);
    reloaded.setRecommended(source, "Fixture/Lost", false);
    pump(http, [&] { return !reloaded.state("official:Fixture/Lost").pending; });
    TEST_CHECK(!reloaded.state("official:Fixture/Lost").recommended);

    // Read-only cache is separate from remembered state and survives a bodyless 304.
    HttpRequest request;
    request.url = "https://api.noctalia.dev/v1/plugin-metrics";
    bool readDone = false;
    std::string etag;
    http.request(request, [&](HttpResponse response) {
      TEST_CHECK(response.transportOk && response.status == 200);
      etag = response.headers.at("etag");
      readDone = true;
    });
    pump(http, [&] { return readDone; });
    readDone = false;
    request.headers = {"If-None-Match: " + etag};
    http.request(request, [&](HttpResponse response) {
      TEST_CHECK(response.transportOk && response.status == 304 && response.body.empty());
      TEST_CHECK(response.headers.at("etag") == etag);
      readDone = true;
    });
    pump(http, [&] { return readDone; });

    const auto shadow = scripting::plugin_paths::localSourceRoot() / "Hidden";
    std::filesystem::create_directories(shadow);
    std::filesystem::copy_file(
        scripting::plugin_paths::gitMaterializedRoot(source) / "Hidden/plugin.toml", shadow / "plugin.toml"
    );
    service.setRecommended(source, "Fixture/Hidden", false);
    TEST_CHECK(service.state("official:Fixture/Hidden").error == RecommendationError::NotRecommendable);
    TEST_CHECK(service.state("official:Fixture/Hidden").recommended);
    TEST_CHECK(!service.state("official:Fixture/Hidden").pending);
    std::filesystem::remove_all(shadow);

    http.setOfflineMode(true);
    readDone = false;
    http.request(request, [&](HttpResponse response) {
      TEST_CHECK(!response.transportOk);
      readDone = true;
    });
    TEST_CHECK(!readDone);
    pump(http, [&] { return readDone; });
    std::ofstream out(FileUtils::configDir() + "/config.toml", std::ios::trunc);
    out << "[shell]\noffline_mode = true\n";
    out.close();
    config.forceReload();
    service.fetchMetrics();
    service.setRecommended(source, "Fixture/Hidden", false);
    TEST_CHECK(!service.state("official:Fixture/Hidden").pending);
    TEST_CHECK(service.state("official:Fixture/Hidden").recommended);
    TEST_CHECK(!service.metricsAvailable());
  }
} // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--api") {
    std::println("{}", scripting::kCurrentPluginApiVersion);
    return 0;
  }
  protocolTests();
  if (argc >= 2 && std::string_view(argv[1]) == "--http") {
    TEST_CHECK(argc == 3);
    httpTests(argv[2]);
  }
}
