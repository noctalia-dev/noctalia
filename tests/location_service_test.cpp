#include "config/config_service.h"
#include "i18n/i18n_service.h"
#include "net/http_client.h"
#include "system/location_service.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

  int g_failures = 0;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::println(stderr, "location_service_test: FAIL: {}", message);
      ++g_failures;
    }
  }

  void writeConfig(const std::filesystem::path& path, std::string_view locationConfig) {
    std::ofstream file(path);
    file << "[location]\n" << locationConfig;
  }

} // namespace

int main() {
  const auto root =
      std::filesystem::temp_directory_path() / ("noctalia-location-service-test-" + std::to_string(::getpid()));
  const auto configHome = root / "config";
  const auto stateHome = root / "state";
  const auto cacheHome = root / "cache";
  const auto configDir = configHome / "noctalia";
  const auto configPath = configDir / "config.toml";

  std::filesystem::remove_all(root);
  std::filesystem::create_directories(configDir);
  std::filesystem::create_directories(stateHome);
  std::filesystem::create_directories(cacheHome);
  ::setenv("NOCTALIA_CONFIG_HOME", configHome.c_str(), 1);
  ::setenv("NOCTALIA_STATE_HOME", stateHome.c_str(), 1);
  ::setenv("XDG_CACHE_HOME", cacheHome.c_str(), 1);

  writeConfig(configPath, "auto_locate = false\naddress = \"\"\nlatitude = 52.52\nlongitude = 13.405\n");
  i18n::Service::instance().init("en");

  {
    ConfigService config;
    HttpClient http;
    http.setOfflineMode(true);
    LocationService location(config, http);
    location.initialize();

    auto resolved = location.resolvedLocation();
    expect(resolved.has_value(), "manual coordinates should provide a location at startup");
    if (resolved.has_value()) {
      TEST_NEAR(resolved->latitude, 52.52, 1e-9);
      TEST_NEAR(resolved->longitude, 13.405, 1e-9);
      expect(resolved->sourceLabel == "Manual", "manual location source was not labelled");
    }
    expect(!location.resolving(), "manual coordinates should not start network resolution");

    int changes = 0;
    location.addChangeCallback([&changes] { ++changes; });

    writeConfig(configPath, "auto_locate = false\naddress = \"\"\nlatitude = 40.7128\nlongitude = -74.006\n");
    config.forceReload();
    resolved = location.resolvedLocation();
    expect(changes == 1, "changing manual coordinates should publish a location change");
    TEST_CHECK(resolved.has_value());
    TEST_NEAR(resolved->latitude, 40.7128, 1e-9);
    TEST_NEAR(resolved->longitude, -74.006, 1e-9);

    writeConfig(configPath, "auto_locate = false\naddress = \"\"\nlatitude = 40.7128\n");
    config.forceReload();
    expect(changes == 2, "removing one manual coordinate should publish a location change");
    expect(!location.resolvedLocation().has_value(), "incomplete manual coordinates should not provide a location");

    writeConfig(configPath, "auto_locate = true\naddress = \"\"\nlatitude = 51.5072\nlongitude = -0.1276\n");
    config.forceReload();
    expect(changes == 3, "enabling automatic location should publish a location change");
    expect(location.resolving(), "automatic location should take priority over manual coordinates");
    expect(!location.resolvedLocation().has_value(), "manual coordinates should not bypass automatic resolution");

    writeConfig(configPath, "auto_locate = false\naddress = \"\"\nlatitude = 51.5072\nlongitude = -0.1276\n");
    config.forceReload();
    resolved = location.resolvedLocation();
    expect(changes == 4, "switching back to manual coordinates should publish a location change");
    TEST_CHECK(resolved.has_value());
    TEST_NEAR(resolved->latitude, 51.5072, 1e-9);
    TEST_NEAR(resolved->longitude, -0.1276, 1e-9);
  }

  std::filesystem::remove_all(root);
  return g_failures == 0 ? 0 : 1;
}
