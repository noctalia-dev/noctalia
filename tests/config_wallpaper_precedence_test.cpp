#include "config/config_service.h"
#include "config/config_types.h"
#include "core/files/resource_paths.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unistd.h>

namespace {

  void checkBundledWallpaperRemainsFallback() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("noctalia-wallpaper-fallback-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "config" / "noctalia");
    std::filesystem::create_directories(root / "state" / "noctalia");
    std::filesystem::create_directories(root / "data");
    ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
    ::setenv("NOCTALIA_STATE_HOME", (root / "state").c_str(), 1);
    ::setenv("NOCTALIA_DATA_HOME", (root / "data").c_str(), 1);

    const auto bundled = paths::assetPath("noctalia-wallpaper.png").string();
    TEST_CHECK(std::filesystem::exists(bundled));
    {
      ConfigService config;
      TEST_CHECK(config.getWallpaperPath("DP-1") == bundled);
      TEST_CHECK(config.markSetupWizardCompleted());
      TEST_CHECK(config.getDefaultWallpaperPath() == bundled);
      TEST_CHECK(config.getPaletteWallpaperPath() == bundled);
      TEST_CHECK(config.getGreeterSyncWallpaperPath() == bundled);
      TEST_CHECK(!config.hasOverride({"wallpaper", "default", "path"}));
      TEST_CHECK(!config.hasOverride({"wallpaper", "last", "path"}));
      TEST_CHECK(config.setOverride({"accessibility", "ui_scale"}, 1.25));
    }
    {
      ConfigService config;
      TEST_CHECK(config.getWallpaperPath("DP-1") == bundled);
      {
        std::ofstream out(root / "config" / "noctalia" / "config.toml");
        out << "[wallpaper.default]\npath = \"/tmp/declared.png\"\n";
      }
      config.forceReload();
      TEST_CHECK(config.getWallpaperPath("DP-1") == "/tmp/declared.png");
      config.setWallpaperPath(std::nullopt, "/tmp/picked.png");
      config.forceReload();
      TEST_CHECK(config.getWallpaperPath("DP-1") == "/tmp/picked.png");
    }
    {
      ConfigService config;
      TEST_CHECK(config.getDefaultWallpaperPath() == "/tmp/picked.png");
    }
    ::unsetenv("NOCTALIA_CONFIG_HOME");
    ::unsetenv("NOCTALIA_STATE_HOME");
    ::unsetenv("NOCTALIA_DATA_HOME");
    std::filesystem::remove_all(root);
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
    TEST_CHECK(!std::filesystem::exists(sidecar));

    ConfigService config;
    TEST_CHECK(config.getDefaultWallpaperPath() == "/tmp/from-config.png");
    TEST_CHECK(config.markSetupWizardCompleted());
    TEST_CHECK(!config.hasOverride({"wallpaper", "default", "path"}));

    TEST_CHECK(config.setOverride({"accessibility", "ui_scale"}, 1.25));
    TEST_CHECK(std::filesystem::exists(sidecar));

    TEST_CHECK(config.getDefaultWallpaperPath() == "/tmp/from-config.png");

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
    TEST_CHECK(config.getDefaultWallpaperPath() == "/tmp/picked.png");

    config.forceReload();
    TEST_CHECK(config.getDefaultWallpaperPath() == "/tmp/picked.png");

    config.setWallpaperPath("DP-9", "/tmp/dp9.png");
    config.forceReload();
    TEST_CHECK(config.getWallpaperPath("DP-9") == "/tmp/dp9.png");
    TEST_CHECK(config.getWallpaperPath("DP-8") == "/tmp/picked.png");

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
    TEST_CHECK(config.config().wallpaper.directory == "/tmp/wp-root/pics");

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
    TEST_CHECK(config.config().theme.mode == ThemeMode::Dark);

    config.addWallpaperFavorite("/tmp/bookmark.png");
    config.forceReload();
    const WallpaperFavorite* bookmark = config.wallpaperFavorite("/tmp/bookmark.png");
    TEST_CHECK(bookmark != nullptr && !bookmark->themeMode.has_value());

    config.applyWallpaperSelection(std::nullopt, "/tmp/bookmark.png", bookmark, {});
    TEST_CHECK(config.config().theme.mode == ThemeMode::Dark);

    config.applyWallpaperSelection(
        std::nullopt, "/tmp/explicit.png", config.wallpaperFavorite("/tmp/explicit.png"), {}
    );
    TEST_CHECK(config.config().theme.mode == ThemeMode::Light);

    ::unsetenv("NOCTALIA_CONFIG_HOME");
    ::unsetenv("NOCTALIA_STATE_HOME");
    ::unsetenv("NOCTALIA_DATA_HOME");
    std::filesystem::remove_all(root);
  }

} // namespace

int main(int argc, char** argv) {
  TEST_CHECK(argc == 2);
  ::setenv("NOCTALIA_ASSETS_DIR", argv[1], 1);
  checkBundledWallpaperRemainsFallback();
  checkConfigSurvivesFirstSidecarWrite();
  checkSidecarPathOutranksConfigFilePath();
  checkDirectoryExpandsEnvVars();
  checkFavoriteThemeModeOverride();

  return 0;
}
