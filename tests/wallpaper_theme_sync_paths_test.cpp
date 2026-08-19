#include "shell/wallpaper/wallpaper_paths.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "wayland/wayland_connection.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::fprintf(stderr, "wallpaper_theme_sync_paths_test: %s\n", message);
      return false;
    }
    return true;
  }

  WaylandOutput makeOutput(std::string connector) {
    return WaylandOutput{.connectorName = std::move(connector), .description = "test"};
  }

} // namespace

int main() {
  WallpaperConfig config{};
  config.themeSync.enabled = true;
  config.themeSync.pathLight = "/wallpapers/day.jpg";
  config.themeSync.pathDark = "/wallpapers/night.jpg";

  const WaylandOutput output = makeOutput("eDP-1");
  bool ok = true;

  ok = expect(
           wallpaper::resolveThemeSyncPath(config, output, ThemeMode::Light) == "/wallpapers/day.jpg",
           "global light binding resolves"
       )
      && ok;
  ok = expect(
           wallpaper::resolveThemeSyncPath(config, output, ThemeMode::Dark) == "/wallpapers/night.jpg",
           "global dark binding resolves"
       )
      && ok;
  ok = expect(
           !wallpaper::resolveThemeSyncPath(config, output, ThemeMode::Light).has_value()
           || wallpaper::hasThemeSyncBinding(config, output, ThemeMode::Light),
           "hasThemeSyncBinding matches global light binding"
       )
      && ok;
  ok = expect(wallpaper::hasGlobalThemeSyncBinding(config, ThemeMode::Light), "global light binding detected")
      && ok;

  config.themeSync.monitorOverrides.push_back(
      WallpaperThemeSyncMonitorOverride{.match = "eDP-1", .pathLight = "/wallpapers/edp-day.jpg", .pathDark = ""}
  );

  ok = expect(
           wallpaper::resolveThemeSyncPath(config, output, ThemeMode::Light) == "/wallpapers/edp-day.jpg",
           "monitor override wins for light mode"
       )
      && ok;
  ok = expect(
           wallpaper::resolveThemeSyncPath(config, output, ThemeMode::Dark) == "/wallpapers/night.jpg",
           "empty monitor override falls back to global dark binding"
       )
      && ok;
  ok = expect(
           wallpaper::themeSyncModeForPath(config, "/wallpapers/edp-day.jpg", "eDP-1") == ThemeMode::Light,
           "theme sync badge resolves light binding for monitor"
       )
      && ok;

  config.themeSync.enabled = false;
  ok = expect(
           !wallpaper::resolveThemeSyncPath(config, output, ThemeMode::Light).has_value(),
           "disabled theme sync returns no binding"
       )
      && ok;

  const auto bindingModeForTab = [](ThemeMode tab, ThemeMode configured, bool isLight) {
    if (tab == ThemeMode::Light || tab == ThemeMode::Dark) {
      return tab;
    }
    return wallpaper::effectiveThemeMode(configured, isLight);
  };

  ok = expect(
           bindingModeForTab(ThemeMode::Auto, ThemeMode::Auto, false) == ThemeMode::Dark,
           "auto tab binds wallpaper picks to resolved dark"
       )
      && ok;
  ok = expect(
           bindingModeForTab(ThemeMode::Auto, ThemeMode::Auto, true) == ThemeMode::Light,
           "auto tab binds wallpaper picks to resolved light"
       )
      && ok;
  ok = expect(
           bindingModeForTab(ThemeMode::Dark, ThemeMode::Auto, true) == ThemeMode::Dark,
           "dark tab always binds to dark"
       )
      && ok;

  {
    WallpaperConfig monitorOnly{};
    monitorOnly.themeSync.enabled = true;
    monitorOnly.themeSync.monitorOverrides.push_back(
        WallpaperThemeSyncMonitorOverride{.match = "DP-1", .pathDark = "/wallpapers/dp-dark.jpg"}
    );
    const WaylandOutput dp1 = makeOutput("DP-1");

    ok = expect(
             wallpaper::resolveThemeSyncPath(monitorOnly, dp1, ThemeMode::Dark) == "/wallpapers/dp-dark.jpg",
             "monitor-only dark binding resolves for that output"
         )
        && ok;
    ok = expect(
             !wallpaper::hasGlobalThemeSyncBinding(monitorOnly, ThemeMode::Dark),
             "monitor-only binding is invisible to global automation guard"
         )
        && ok;
    ok = expect(
             !wallpaper::themeSyncModeForPath(monitorOnly, "/wallpapers/dp-dark.jpg", std::nullopt).has_value(),
             "all-monitors badge lookup ignores monitor-only bindings"
         )
        && ok;
  }

  {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("noctalia-theme-sync-bind-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "config" / "noctalia");
    std::filesystem::create_directories(root / "state" / "noctalia");
    ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
    ::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);

    ConfigService config;
    constexpr std::string_view kPath = "/wallpapers/dp-dark.jpg";
    wallpaper::setThemeSyncBinding(config, std::string{"DP-1"}, ThemeMode::Dark, kPath, {});

    ok = expect(
             config.config().wallpaper.themeSync.pathDark == kPath,
             "single-monitor bind also updates global path_dark"
         )
        && ok;
    ok = expect(config.config().wallpaper.themeSync.monitorOverrides.size() == 1, "monitor override stored")
        && ok;
    ok = expect(
             config.config().wallpaper.themeSync.monitorOverrides.front().match == "DP-1"
                 && config.config().wallpaper.themeSync.monitorOverrides.front().pathDark == kPath,
             "monitor override matches single-monitor bind"
         )
        && ok;
    ok = expect(
             wallpaper::hasGlobalThemeSyncBinding(config.config().wallpaper, ThemeMode::Dark),
             "global automation guard sees single-monitor bind"
         )
        && ok;

    std::filesystem::remove_all(root);
  }

  return ok ? 0 : 1;
}
