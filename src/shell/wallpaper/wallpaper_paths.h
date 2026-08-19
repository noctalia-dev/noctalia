#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class ConfigService;

struct WallpaperConfig;
struct WallpaperMonitorOverride;
struct WallpaperThemeSyncConfig;
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

  [[nodiscard]] bool hasThemeSyncBinding(const WallpaperConfig& config, const WaylandOutput& output, ThemeMode mode);

  [[nodiscard]] bool hasGlobalThemeSyncBinding(const WallpaperConfig& config, ThemeMode mode);

  // Returns light/dark when path is bound for theme sync on the given output scope.
  [[nodiscard]] std::optional<ThemeMode> themeSyncModeForPath(
      const WallpaperConfig& config, std::string_view path, const std::optional<std::string>& connector
  );

  void setThemeSyncBinding(
      ConfigService& config, const std::optional<std::string>& connector, ThemeMode mode, std::string_view path,
      std::span<const std::string> allConnectors = {}
  );

  // Returns output connectors to mirror when binding for all outputs; empty for a single monitor.
  [[nodiscard]] std::vector<std::string> themeSyncConnectorsForGlobalBinding(
      const std::optional<std::string>& connector, std::span<const std::string> allOutputConnectors
  );

  [[nodiscard]] bool
  shouldSeedThemeSyncBinding(const WallpaperThemeSyncConfig& themeSync, bool resolvedIsLight) noexcept;

  [[nodiscard]] ThemeMode themeSyncModeForResolvedAppearance(bool resolvedIsLight) noexcept;

  void bindThemeSyncForManualPick(
      ConfigService& config, const std::optional<std::string>& connector, bool resolvedIsLight, std::string_view path,
      std::span<const std::string> allOutputConnectors
  );

  void seedThemeSyncBindingIfNeeded(
      ConfigService& config, bool resolvedIsLight, std::string_view wallpaperPath,
      std::span<const std::string> allOutputConnectors
  );

} // namespace wallpaper
