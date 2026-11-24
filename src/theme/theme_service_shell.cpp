#include "theme/theme_service.h"

#include "config/config_service.h"
#include "theme/custom_palettes.h"
#include "theme/scheme.h"

#include <string>
#include <utility>

namespace nythera::theme {

  void ThemeService::toggleLightDark() {
    const auto next = m_isLightMode ? ThemeMode::Dark : ThemeMode::Light;
    // Persist via ConfigService → StateService. The resulting overrides-change
    // callback rebuilds the Config and fires the reload callbacks, which call
    // ThemeService::onConfigReload() to transition to the new palette.
    m_config.setThemeMode(next);
  }

  void ThemeService::cycleMode() {
    ThemeMode next = ThemeMode::Dark;
    switch (configuredMode()) {
    case ThemeMode::Dark:
      next = ThemeMode::Light;
      break;
    case ThemeMode::Light:
      next = ThemeMode::Auto;
      break;
    case ThemeMode::Auto:
      next = ThemeMode::Dark;
      break;
    }
    m_config.setThemeMode(next);
  }

  bool ThemeService::saveWallpaperPaletteAsCustom(std::string* paletteNameOut, std::string* errorOut) {
    auto fail = [errorOut](std::string message) {
      if (errorOut != nullptr) {
        *errorOut = std::move(message);
      }
      return false;
    };

    const auto& cfg = m_config.config().theme;
    if (cfg.source != PaletteSource::Wallpaper) {
      return fail("palette source is not wallpaper");
    }

    const std::string wallpaperPath = m_config.getPaletteWallpaperPath();
    const auto generated = resolveWallpaperGenerated(cfg, wallpaperPath);
    if (!generated) {
      return fail("failed to generate palette from wallpaper");
    }

    const std::string name = allocateCustomPaletteName(wallpaperPath, cfg.wallpaperScheme);
    if (name.empty()) {
      return fail("failed to allocate custom palette name");
    }

    if (!saveCustomPaletteFromGenerated(name, *generated, errorOut)) {
      return false;
    }

    if (!m_config.setThemeColorScheme(PaletteSource::Custom, name)) {
      return fail("failed to switch palette source to custom");
    }

    if (paletteNameOut != nullptr) {
      *paletteNameOut = name;
    }
    return true;
  }

} // namespace nythera::theme
