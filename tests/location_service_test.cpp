#include "config/config_service.h"
#include "i18n/i18n_service.h"
#include "net/http_client.h"
#include "system/location_service.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

  std::filesystem::path g_root;

  void removeRoot() {
    std::error_code ec;
    std::filesystem::remove_all(g_root, ec);
  }

  void writeConfig(const std::filesystem::path& path, std::string_view locationConfig) {
    std::ofstream file(path);
    file << "[location]\n" << locationConfig;
  }

} // namespace

int main() {
  g_root = std::filesystem::temp_directory_path() / ("noctalia-location-service-test-" + std::to_string(::getpid()));
  const auto& root = g_root;
  const auto configHome = root / "config";
  const auto stateHome = root / "state";
  const auto cacheHome = root / "cache";
  const auto configDir = configHome / "noctalia";
  const auto configPath = configDir / "config.toml";

  std::filesystem::remove_all(root);
  std::filesystem::create_directories(configDir);
  std::atexit(removeRoot);
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
    TEST_CHECK(resolved.has_value());
    TEST_NEAR(resolved->latitude, 52.52, 1e-9);
    TEST_NEAR(resolved->longitude, 13.405, 1e-9);
    TEST_CHECK(resolved->sourceLabel == "Manual");
    TEST_CHECK(!location.resolving());

    int changes = 0;
    location.addChangeCallback([&changes] { ++changes; });

    writeConfig(configPath, "auto_locate = false\naddress = \"\"\nlatitude = 40.7128\nlongitude = -74.006\n");
    config.forceReload();
    resolved = location.resolvedLocation();
    TEST_CHECK(changes == 1);
    TEST_CHECK(resolved.has_value());
    TEST_NEAR(resolved->latitude, 40.7128, 1e-9);
    TEST_NEAR(resolved->longitude, -74.006, 1e-9);

    writeConfig(configPath, "auto_locate = false\naddress = \"\"\nlatitude = 40.7128\n");
    config.forceReload();
    TEST_CHECK(changes == 2);
    TEST_CHECK(!location.resolvedLocation().has_value());

    writeConfig(configPath, "auto_locate = true\naddress = \"\"\nlatitude = 51.5072\nlongitude = -0.1276\n");
    config.forceReload();
    TEST_CHECK(changes == 3);
    TEST_CHECK(location.resolving());
    TEST_CHECK(!location.resolvedLocation().has_value());

    writeConfig(configPath, "auto_locate = false\naddress = \"\"\nlatitude = 51.5072\nlongitude = -0.1276\n");
    config.forceReload();
    resolved = location.resolvedLocation();
    TEST_CHECK(changes == 4);
    TEST_CHECK(resolved.has_value());
    TEST_NEAR(resolved->latitude, 51.5072, 1e-9);
    TEST_NEAR(resolved->longitude, -0.1276, 1e-9);
  }

  return 0;
}
