#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct WallpaperConfig;
struct WallpaperMonitorOverride;
struct WaylandOutput;
enum class ThemeMode : std::uint8_t;

namespace wallpaper {

  // Maps theme.mode=auto to the currently resolved light/dark appearance.
  [[nodiscard]] ThemeMode effectiveThemeMode(ThemeMode mode, bool isLight) noexcept;

  // Resolves a user-supplied wallpaper argument (CLI, plugin) to a value ready to persist:
  // a "color:" literal as-is, or an existing regular file's canonical path. nullopt if
  // neither applies. callerCwd anchors relative paths for IPC callers outside our cwd.
  [[nodiscard]] std::optional<std::string>
  resolveWallpaperImagePath(std::string_view path, std::optional<std::string_view> callerCwd = std::nullopt);

  [[nodiscard]] const WallpaperMonitorOverride*
  findWallpaperMonitorOverride(const WallpaperConfig& config, const WaylandOutput& output);

  [[nodiscard]] std::string
  resolveWallpaperDirectory(const WallpaperConfig& config, const WaylandOutput& output, ThemeMode mode);

  [[nodiscard]] std::string resolveGlobalWallpaperDirectory(const WallpaperConfig& config, ThemeMode mode);

} // namespace wallpaper
