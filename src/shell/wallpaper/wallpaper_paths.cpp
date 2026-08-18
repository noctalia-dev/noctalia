#include "shell/wallpaper/wallpaper_paths.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "util/file_utils.h"

#include <optional>
#include <utility>
#include <vector>

ThemeMode wallpaper::effectiveThemeMode(ThemeMode mode, bool isLight) noexcept {
  if (mode == ThemeMode::Auto) {
    return isLight ? ThemeMode::Light : ThemeMode::Dark;
  }
  return mode;
}

const WallpaperMonitorOverride*
wallpaper::findWallpaperMonitorOverride(const WallpaperConfig& config, const WaylandOutput& output) {
  for (const auto& ovr : config.monitorOverrides) {
    if (outputMatchesSelector(ovr.match, output)) {
      return &ovr;
    }
  }
  return nullptr;
}

std::string
wallpaper::resolveWallpaperDirectory(const WallpaperConfig& config, const WaylandOutput& output, ThemeMode mode) {
  if (config.perMonitorDirectories) {
    if (const auto* ovr = findWallpaperMonitorOverride(config, output); ovr != nullptr) {
      if (mode == ThemeMode::Light && ovr->directoryLight.has_value() && !ovr->directoryLight->empty()) {
        return *ovr->directoryLight;
      }
      if (mode == ThemeMode::Dark && ovr->directoryDark.has_value() && !ovr->directoryDark->empty()) {
        return *ovr->directoryDark;
      }
      if (ovr->directory.has_value() && !ovr->directory->empty()) {
        return *ovr->directory;
      }
    }
  }
  return resolveGlobalWallpaperDirectory(config, mode);
}

std::string wallpaper::resolveGlobalWallpaperDirectory(const WallpaperConfig& config, ThemeMode mode) {
  if (mode == ThemeMode::Light && !config.directoryLight.empty()) {
    return config.directoryLight;
  }
  if (mode == ThemeMode::Dark && !config.directoryDark.empty()) {
    return config.directoryDark;
  }
  if (!config.directory.empty()) {
    return config.directory;
  }
  return FileUtils::defaultPicturesDirectory().string();
}

namespace {

  [[nodiscard]] const WallpaperThemeSyncMonitorOverride*
  findThemeSyncMonitorOverride(const WallpaperConfig& config, const WaylandOutput& output) {
    for (const auto& ovr : config.themeSync.monitorOverrides) {
      if (outputMatchesSelector(ovr.match, output)) {
        return &ovr;
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::optional<std::string>
  pickThemeSyncPath(const std::string& pathLight, const std::string& pathDark, ThemeMode mode) {
    if (mode == ThemeMode::Light) {
      return pathLight.empty() ? std::nullopt : std::make_optional(pathLight);
    }
    return pathDark.empty() ? std::nullopt : std::make_optional(pathDark);
  }

} // namespace

std::optional<std::string>
wallpaper::resolveThemeSyncPath(const WallpaperConfig& config, const WaylandOutput& output, ThemeMode mode) {
  if (!config.themeSync.enabled) {
    return std::nullopt;
  }

  if (const auto* ovr = findThemeSyncMonitorOverride(config, output)) {
    if (auto path = pickThemeSyncPath(ovr->pathLight, ovr->pathDark, mode)) {
      return path;
    }
  }

  return pickThemeSyncPath(config.themeSync.pathLight, config.themeSync.pathDark, mode);
}

bool wallpaper::hasThemeSyncBinding(const WallpaperConfig& config, const WaylandOutput& output, ThemeMode mode) {
  const auto path = resolveThemeSyncPath(config, output, mode);
  return path.has_value() && !path->empty();
}

bool wallpaper::hasGlobalThemeSyncBinding(const WallpaperConfig& config, ThemeMode mode) {
  if (!config.themeSync.enabled) {
    return false;
  }
  if (mode == ThemeMode::Light) {
    return !config.themeSync.pathLight.empty();
  }
  return !config.themeSync.pathDark.empty();
}

std::optional<ThemeMode> wallpaper::themeSyncModeForPath(
    const WallpaperConfig& config, std::string_view path, const std::optional<std::string>& connector
) {
  if (!config.themeSync.enabled || path.empty()) {
    return std::nullopt;
  }

  const std::string normalized = FileUtils::normalizeWallpaperPath(path);
  const auto matches = [&](const std::string& bound) {
    return !bound.empty() && FileUtils::normalizeWallpaperPath(bound) == normalized;
  };

  if (connector.has_value() && !connector->empty()) {
    for (const auto& ovr : config.themeSync.monitorOverrides) {
      if (ovr.match != *connector) {
        continue;
      }
      if (matches(ovr.pathLight)) {
        return ThemeMode::Light;
      }
      if (matches(ovr.pathDark)) {
        return ThemeMode::Dark;
      }
      return std::nullopt;
    }
  }

  if (matches(config.themeSync.pathLight)) {
    return ThemeMode::Light;
  }
  if (matches(config.themeSync.pathDark)) {
    return ThemeMode::Dark;
  }
  return std::nullopt;
}

void wallpaper::setThemeSyncBinding(
    ConfigService& config, const std::optional<std::string>& connector, ThemeMode mode, std::string_view path,
    std::span<const std::string> allConnectors
) {
  if (path.empty() || (mode != ThemeMode::Light && mode != ThemeMode::Dark)) {
    return;
  }

  const std::string key = mode == ThemeMode::Light ? "path_light" : "path_dark";
  std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
  if (!config.config().wallpaper.themeSync.enabled) {
    overrides.emplace_back(std::vector<std::string>{"wallpaper", "theme_sync", "enabled"}, true);
  }
  if (connector.has_value() && !connector->empty()) {
    overrides.emplace_back(
        std::vector<std::string>{"wallpaper", "theme_sync", "monitor", *connector, key}, std::string(path)
    );
  } else {
    overrides.emplace_back(std::vector<std::string>{"wallpaper", "theme_sync", key}, std::string(path));
    for (const auto& mon : allConnectors) {
      if (mon.empty()) {
        continue;
      }
      overrides.emplace_back(
          std::vector<std::string>{"wallpaper", "theme_sync", "monitor", mon, key}, std::string(path)
      );
    }
  }
  (void)config.setOverrides(std::move(overrides));
}
