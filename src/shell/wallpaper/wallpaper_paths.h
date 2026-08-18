#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

class ConfigService;

struct WallpaperConfig;
struct WallpaperMonitorOverride;
struct WaylandOutput;
enum class ThemeMode : std::uint8_t;

namespace wallpaper {

  // Maps theme.mode=auto to the currently resolved light/dark appearance.
  [[nodiscard]] ThemeMode effectiveThemeMode(ThemeMode mode, bool isLight) noexcept;

  [[nodiscard]] const WallpaperMonitorOverride*
  findWallpaperMonitorOverride(const WallpaperConfig& config, const WaylandOutput& output);

  [[nodiscard]] std::string
  resolveWallpaperDirectory(const WallpaperConfig& config, const WaylandOutput& output, ThemeMode mode);

  [[nodiscard]] std::string resolveGlobalWallpaperDirectory(const WallpaperConfig& config, ThemeMode mode);

  // Returns a bound wallpaper path for the resolved theme mode, or nullopt when theme sync
  // is disabled or no binding exists for this output/mode.
  [[nodiscard]] std::optional<std::string>
  resolveThemeSyncPath(const WallpaperConfig& config, const WaylandOutput& output, ThemeMode mode);

  [[nodiscard]] bool
  hasThemeSyncBinding(const WallpaperConfig& config, const WaylandOutput& output, ThemeMode mode);

  [[nodiscard]] bool hasGlobalThemeSyncBinding(const WallpaperConfig& config, ThemeMode mode);

  // Returns light/dark when path is bound for theme sync on the given output scope.
  [[nodiscard]] std::optional<ThemeMode> themeSyncModeForPath(
      const WallpaperConfig& config, std::string_view path, const std::optional<std::string>& connector
  );

  void setThemeSyncBinding(
      ConfigService& config, const std::optional<std::string>& connector, ThemeMode mode, std::string_view path,
      std::span<const std::string> allConnectors = {}
  );

} // namespace wallpaper
