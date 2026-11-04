#pragma once

#include "launcher/launcher_provider.h"

class ConfigService;
class WaylandConnection;

namespace noctalia::theme {
  class ThemeService;
}

class WallpaperProvider : public LauncherProvider {
public:
  WallpaperProvider(
      ConfigService* config, WaylandConnection* wayland, noctalia::theme::ThemeService* themeService = nullptr
  );

  std::string_view defaultPrefix() const override { return "wall"; }
  std::string_view id() const override { return "Wallpaper"; }
  std::string displayName() const override;
  std::string_view defaultGlyphName() const override { return "wallpaper-selector"; }
  bool supportsGrid() const override { return true; }
  bool trackUsage() const override { return true; }

  std::vector<LauncherResult> query(std::string_view text) const override;

  bool activate(const LauncherResult& result) override;

private:
  ConfigService* m_config = nullptr;
  WaylandConnection* m_wayland = nullptr;
  noctalia::theme::ThemeService* m_themeService = nullptr;
};
