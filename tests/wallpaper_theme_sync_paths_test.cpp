#include "shell/wallpaper/wallpaper_paths.h"

#include "config/config_types.h"
#include "wayland/wayland_connection.h"

#include <cstdio>
#include <string>

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

  return ok ? 0 : 1;
}
