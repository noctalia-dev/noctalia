#include "config/config_service.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

  int g_failures = 0;

  void expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "config_wallpaper_precedence_test: FAIL: {}", message);
      ++g_failures;
    }
  }

  // Check that wallpaper setting from config.toml isn't dropped when state/settings.toml
  // is written for the first time.
  void checkConfigSurvivesFirstSidecarWrite() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("noctalia-wallpaper-config-file-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "config" / "noctalia");
    std::filesystem::create_directories(root / "state" / "noctalia");
    std::filesystem::create_directories(root / "data");
    ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
    ::setenv("NOCTALIA_STATE_HOME", (root / "state").c_str(), 1);
    ::setenv("NOCTALIA_DATA_HOME", (root / "data").c_str(), 1);

    {
      std::ofstream out(root / "config" / "noctalia" / "config.toml", std::ios::trunc);
      out << "[wallpaper]\nenabled = true\n\n[wallpaper.default]\npath = \"/tmp/from-config.png\"\n";
    }
    const auto sidecar = root / "state" / "noctalia" / "settings.toml";
    expect(!std::filesystem::exists(sidecar), "state dir should start without settings.toml");

    ConfigService config;
    expect(config.getDefaultWallpaperPath() == "/tmp/from-config.png", "path was not read from config.toml");

    expect(config.setOverride({"accessibility", "ui_scale"}, 1.25), "setOverride failed");
    expect(std::filesystem::exists(sidecar), "setOverride did not create settings.toml");

    expect(
        config.getDefaultWallpaperPath() == "/tmp/from-config.png",
        "config.toml path was dropped by the first sidecar write"
    );

    ::unsetenv("NOCTALIA_CONFIG_HOME");
    ::unsetenv("NOCTALIA_STATE_HOME");
    ::unsetenv("NOCTALIA_DATA_HOME");
    std::filesystem::remove_all(root);
  }

  // State settings loads last, so it should outranks config.toml
  void checkSidecarPathOutranksConfigFilePath() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("noctalia-wallpaper-sidecar-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "config" / "noctalia");
    std::filesystem::create_directories(root / "state" / "noctalia");
    std::filesystem::create_directories(root / "data");
    ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
    ::setenv("NOCTALIA_STATE_HOME", (root / "state").c_str(), 1);
    ::setenv("NOCTALIA_DATA_HOME", (root / "data").c_str(), 1);

    {
      std::ofstream out(root / "config" / "noctalia" / "config.toml", std::ios::trunc);
      out << "[wallpaper]\nenabled = true\n\n[wallpaper.default]\npath = \"/tmp/from-config.png\"\n";
    }
    ConfigService config;
    config.setWallpaperPath(std::nullopt, "/tmp/picked.png");
    expect(config.getDefaultWallpaperPath() == "/tmp/picked.png", "sidecar path did not win once set");

    config.forceReload();
    expect(config.getDefaultWallpaperPath() == "/tmp/picked.png", "sidecar path did not survive a reload");

    config.setWallpaperPath("DP-9", "/tmp/dp9.png");
    config.forceReload();
    expect(config.getWallpaperPath("DP-9") == "/tmp/dp9.png", "per-monitor path did not survive a reload");
    expect(config.getWallpaperPath("DP-8") == "/tmp/picked.png", "unconfigured monitor did not fall back to default");

    ::unsetenv("NOCTALIA_CONFIG_HOME");
    ::unsetenv("NOCTALIA_STATE_HOME");
    ::unsetenv("NOCTALIA_DATA_HOME");
    std::filesystem::remove_all(root);
  }

} // namespace

int main() {
  checkConfigSurvivesFirstSidecarWrite();
  checkSidecarPathOutranksConfigFilePath();

  if (g_failures == 0) {
    std::println("config_wallpaper_precedence_test: all checks passed");
  }
  return g_failures == 0 ? 0 : 1;
}
