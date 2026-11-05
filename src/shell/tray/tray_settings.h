#pragma once

#include "shell/bar/widgets/tray_widget_definition.h"

#include <optional>
#include <string_view>

class ConfigService;

namespace tray {

  // Tray surfaces opened without a source widget use this config name.
  inline constexpr std::string_view kCanonicalTrayWidgetName = "tray";

  struct ResolvedTrayOptions {
    TrayWidget::Options options;
    std::optional<float> drawerItemSize;
    std::optional<float> drawerBackgroundOpacity;
  };

  ResolvedTrayOptions resolvedTrayOptions(
      const ConfigService& config, const TrayWidgetDefinitionContext& context = {},
      std::string_view widgetName = kCanonicalTrayWidgetName
  );
  WidgetBarCapsuleSpec resolvedTrayCapsuleSpec(
      const ConfigService& config, const BarConfig& bar, std::string_view widgetName = kCanonicalTrayWidgetName
  );

} // namespace tray
