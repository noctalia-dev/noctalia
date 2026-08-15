#pragma once

#include "shell/bar/widgets/tray_widget_definition.h"

#include <optional>
#include <string_view>

class ConfigService;

namespace tray {

  // Canonical settings for the singleton drawer and menu surfaces. Bar-embedded
  // tray widgets resolve their own, possibly differently named, entries.
  inline constexpr std::string_view kCanonicalTrayWidgetName = "tray";

  struct ResolvedTrayOptions {
    TrayWidget::Options options;
    std::optional<float> drawerItemSize;
  };

  [[nodiscard]] ResolvedTrayOptions
  resolvedTrayOptions(const ConfigService& config, const TrayWidgetDefinitionContext& context = {});
  [[nodiscard]] WidgetBarCapsuleSpec resolvedTrayCapsuleSpec(const ConfigService& config, const BarConfig& bar);

} // namespace tray
