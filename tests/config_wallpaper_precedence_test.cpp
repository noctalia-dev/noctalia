#include "config/config_service.h"
#include "config/config_types.h"

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

  // Path fields document that ~ and $VARS expand on read (see field.h
  // pathStringField). Regression guard: wallpaper.directory must expand $VARS.
  void checkDirectoryExpandsEnvVars() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("noctalia-wallpaper-envvar-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "config" / "noctalia");
    std::filesystem::create_directories(root / "state" / "noctalia");
    std::filesystem::create_directories(root / "data");
    ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
    ::setenv("NOCTALIA_STATE_HOME", (root / "state").c_str(), 1);
    ::setenv("NOCTALIA_DATA_HOME", (root / "data").c_str(), 1);
    ::setenv("NOCTALIA_WP_TEST", "/tmp/wp-root", 1);

    {
      std::ofstream out(root / "config" / "noctalia" / "config.toml", std::ios::trunc);
      out << "[wallpaper]\nenabled = true\ndirectory = \"$NOCTALIA_WP_TEST/pics\"\n";
    }

    ConfigService config;
    expect(
        config.config().wallpaper.directory == "/tmp/wp-root/pics",
        "wallpaper.directory did not expand $NOCTALIA_WP_TEST"
    );

    ::unsetenv("NOCTALIA_WP_TEST");
    ::unsetenv("NOCTALIA_CONFIG_HOME");
    ::unsetenv("NOCTALIA_STATE_HOME");
    ::unsetenv("NOCTALIA_DATA_HOME");
    std::filesystem::remove_all(root);
  }

  // A favorite starred without a preset must not touch the global theme mode when applied;
  // one with an explicit theme_mode must restore it.
  void checkFavoriteThemeModeOverride() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("noctalia-wallpaper-favorite-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "config" / "noctalia");
    std::filesystem::create_directories(root / "state" / "noctalia");
    std::filesystem::create_directories(root / "data");
    ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
    ::setenv("NOCTALIA_STATE_HOME", (root / "state").c_str(), 1);
    ::setenv("NOCTALIA_DATA_HOME", (root / "data").c_str(), 1);

    {
      std::ofstream out(root / "state" / "noctalia" / "settings.toml", std::ios::trunc);
      out << "[theme]\nmode = \"dark\"\n\n"
             "[[wallpaper.favorite]]\npath = \"/tmp/explicit.png\"\ntheme_mode = \"light\"\n";
    }
    ConfigService config;
    expect(config.config().theme.mode == ThemeMode::Dark, "global theme mode was not read");

    config.addWallpaperFavorite("/tmp/bookmark.png");
    config.forceReload();
    const WallpaperFavorite* bookmark = config.wallpaperFavorite("/tmp/bookmark.png");
    expect(bookmark != nullptr && !bookmark->themeMode.has_value(), "path-only favorite gained a theme mode");

    config.applyWallpaperSelection(std::nullopt, "/tmp/bookmark.png", bookmark, {});
    expect(config.config().theme.mode == ThemeMode::Dark, "path-only favorite changed the global theme mode");

    config.applyWallpaperSelection(
        std::nullopt, "/tmp/explicit.png", config.wallpaperFavorite("/tmp/explicit.png"), {}
    );
    expect(config.config().theme.mode == ThemeMode::Light, "favorite theme_mode was not applied");

    ::unsetenv("NOCTALIA_CONFIG_HOME");
    ::unsetenv("NOCTALIA_STATE_HOME");
    ::unsetenv("NOCTALIA_DATA_HOME");
    std::filesystem::remove_all(root);
  }

  // Temp config/state/data roots wired through the NOCTALIA_*_HOME env vars.
  struct TestRoot {
    std::filesystem::path root;

    explicit TestRoot(const std::string& name, std::string_view configToml = {})
        : root(std::filesystem::temp_directory_path() / (name + std::to_string(::getpid()))) {
      std::filesystem::remove_all(root);
      std::filesystem::create_directories(root / "config" / "noctalia");
      std::filesystem::create_directories(root / "state" / "noctalia");
      std::filesystem::create_directories(root / "data");
      ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
      ::setenv("NOCTALIA_STATE_HOME", (root / "state").c_str(), 1);
      ::setenv("NOCTALIA_DATA_HOME", (root / "data").c_str(), 1);
      if (!configToml.empty()) {
        std::ofstream out(root / "config" / "noctalia" / "config.toml", std::ios::trunc);
        out << configToml;
      }
    }

    ~TestRoot() {
      ::unsetenv("NOCTALIA_CONFIG_HOME");
      ::unsetenv("NOCTALIA_STATE_HOME");
      ::unsetenv("NOCTALIA_DATA_HOME");
      std::error_code ec;
      std::filesystem::permissions(
          root / "state" / "noctalia", std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec
      );
      std::filesystem::remove_all(root, ec);
    }
  };

  // The lock screen inherits each output's own desktop wallpaper until an override is set, and the
  // override applies to every output and survives a reload.
  void checkLockscreenWallpaperResolution() {
    TestRoot env("noctalia-lockscreen-wallpaper-resolve-");
    ConfigService config;
    config.setWallpaperPath(std::nullopt, "/tmp/default.png");
    config.setWallpaperPath("DP-1", "/tmp/dp1.png");

    expect(config.getLockscreenWallpaperPath("DP-1") == "/tmp/dp1.png", "output did not inherit its own wallpaper");
    expect(config.getLockscreenWallpaperPath("DP-2") == "/tmp/default.png", "unknown output did not use default");
    expect(config.getLockscreenWallpaperPath("") == "/tmp/default.png", "empty connector did not use default");

    expect(config.setLockscreenWallpaperPath("/tmp/lock.png"), "setLockscreenWallpaperPath failed");
    expect(config.getLockscreenWallpaperPath("DP-1") == "/tmp/lock.png", "override did not apply to DP-1");
    expect(config.getLockscreenWallpaperPath("DP-2") == "/tmp/lock.png", "override did not apply to DP-2");

    config.forceReload();
    expect(config.getLockscreenWallpaperPath("DP-1") == "/tmp/lock.png", "override did not survive a reload");
  }

  // Clearing removes the override and falls back to config.toml, else the desktop wallpaper.
  // Clearing again succeeds as a no-op.
  void checkLockscreenWallpaperClear() {
    {
      TestRoot env("noctalia-lockscreen-wallpaper-clear-");
      ConfigService config;
      config.setWallpaperPath(std::nullopt, "/tmp/default.png");
      expect(config.setLockscreenWallpaperPath("/tmp/lock.png"), "setLockscreenWallpaperPath failed");
      expect(config.clearLockscreenWallpaperPath(), "clearLockscreenWallpaperPath failed");
      expect(
          config.getLockscreenWallpaperPath("") == "/tmp/default.png", "clear did not restore the desktop wallpaper"
      );
      expect(config.clearLockscreenWallpaperPath(), "clearing an already clear override did not succeed");
    }
    {
      TestRoot env("noctalia-lockscreen-wallpaper-config-", "[lockscreen]\nwallpaper = \"/tmp/from-config.png\"\n");
      ConfigService config;
      config.setWallpaperPath(std::nullopt, "/tmp/default.png");
      expect(config.getLockscreenWallpaperPath("") == "/tmp/from-config.png", "config.toml wallpaper was not read");
      expect(config.setLockscreenWallpaperPath("/tmp/lock.png"), "setLockscreenWallpaperPath failed");
      expect(config.getLockscreenWallpaperPath("") == "/tmp/lock.png", "override did not beat config.toml");
      expect(config.clearLockscreenWallpaperPath(), "clearLockscreenWallpaperPath failed");
      expect(
          config.getLockscreenWallpaperPath("") == "/tmp/from-config.png",
          "clear did not fall back to the config.toml wallpaper"
      );
    }
  }

  // A failed settings write is reported and leaves the effective wallpaper unchanged.
  void checkLockscreenWallpaperSaveFailure() {
    if (::geteuid() == 0) {
      return;
    }
    TestRoot env("noctalia-lockscreen-wallpaper-readonly-");
    ConfigService config;
    config.setWallpaperPath(std::nullopt, "/tmp/default.png");
    expect(config.setLockscreenWallpaperPath("/tmp/lock.png"), "setLockscreenWallpaperPath failed");

    std::filesystem::permissions(
        env.root / "state" / "noctalia", std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace
    );
    expect(!config.setLockscreenWallpaperPath("/tmp/other.png"), "set reported success on an unwritable settings dir");
    expect(!config.clearLockscreenWallpaperPath(), "clear reported success on an unwritable settings dir");
    expect(
        config.getLockscreenWallpaperPath("") == "/tmp/lock.png", "failed write changed the effective lock wallpaper"
    );
  }

} // namespace

int main() {
  checkConfigSurvivesFirstSidecarWrite();
  checkSidecarPathOutranksConfigFilePath();
  checkDirectoryExpandsEnvVars();
  checkFavoriteThemeModeOverride();
  checkLockscreenWallpaperResolution();
  checkLockscreenWallpaperClear();
  checkLockscreenWallpaperSaveFailure();

  if (g_failures == 0) {
    std::println("config_wallpaper_precedence_test: all checks passed");
  }
  return g_failures == 0 ? 0 : 1;
}
