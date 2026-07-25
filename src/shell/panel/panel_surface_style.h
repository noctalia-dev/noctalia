#pragma once

#include "config/config_service.h"
#include "config/config_types.h"
#include "shell/surface/shadow.h"

#include <algorithm>
#include <cstdint>

// Sizing and styling shared by the panel surfaces: PanelManager's single active
// panel and PersistentPanelHost's long-lived ones.
namespace shell::panel_surface {

  // Extra margin around the shadow bleed. The shadow is drawn inside the surface,
  // so a surface sized to the bleed alone clips its own outermost falloff row.
  inline constexpr std::int32_t kShadowSafetyPadding = 2;

  [[nodiscard]] inline shell::surface_shadow::Bleed
  bleed(bool hasDecoration, const ShellConfig::ShadowConfig& shadow) noexcept {
    auto out = shell::surface_shadow::bleed(hasDecoration, shadow);
    if (shell::surface_shadow::enabled(hasDecoration, shadow)) {
      out.left += kShadowSafetyPadding;
      out.right += kShadowSafetyPadding;
      out.up += kShadowSafetyPadding;
      out.down += kShadowSafetyPadding;
    }
    return out;
  }

  [[nodiscard]] inline std::uint32_t
  surfaceExtent(std::uint32_t contentSize, std::int32_t before, std::int32_t after) noexcept {
    const auto total =
        static_cast<std::int64_t>(contentSize) + static_cast<std::int64_t>(before) + static_cast<std::int64_t>(after);
    return static_cast<std::uint32_t>(std::max<std::int64_t>(1, total));
  }

  [[nodiscard]] inline float contentScale(const ConfigService* configService) noexcept {
    if (configService == nullptr) {
      return 1.0f;
    }
    return std::max(0.1f, configService->config().accessibility.uiScale);
  }

  [[nodiscard]] inline float backgroundOpacity(const ConfigService* configService) noexcept {
    const auto mode =
        configService != nullptr ? configService->config().shell.panel.transparencyMode : PanelTransparencyMode::Solid;
    return detachedPanelBackgroundOpacityForTransparencyMode(mode);
  }

  [[nodiscard]] inline float cardOpacity(const ConfigService* configService, float panelBackgroundOpacity) noexcept {
    const auto mode =
        configService != nullptr ? configService->config().shell.panel.transparencyMode : PanelTransparencyMode::Solid;
    return panelCardOpacityForTransparencyMode(mode, panelBackgroundOpacity);
  }

} // namespace shell::panel_surface
