#include "shell/tray/tray_settings.h"

#include "config/config_service.h"

#include <string>
namespace {

  const WidgetConfig* trayWidgetConfig(const ConfigService& config, std::string_view widgetName) {
    const auto& widgets = config.config().widgets;
    const auto configEntry = widgets.find(std::string(widgetName));
    return configEntry != widgets.end() ? &configEntry->second : nullptr;
  }

} // namespace

namespace tray {

  ResolvedTrayOptions resolvedTrayOptions(
      const ConfigService& config, const TrayWidgetDefinitionContext& context, std::string_view widgetName
  ) {
    const WidgetConfig* widgetConfig = trayWidgetConfig(config, widgetName);

    ResolvedTrayOptions resolved{
        .options = trayWidgetDefinition().resolve(widgetConfig, widgetName, context),
    };
    // The drawer distinguishes an omitted item size from the schema default. Keep
    // that single presence check beside the definition-owned resolution.
    if (widgetConfig != nullptr && widgetConfig->hasSetting("drawer_item_size")) {
      resolved.drawerItemSize = static_cast<float>(resolved.options.drawerItemSize);
    }
    if (widgetConfig != nullptr && widgetConfig->hasSetting("drawer_background_opacity")) {
      resolved.drawerBackgroundOpacity =
          std::clamp(static_cast<float>(resolved.options.drawerBackgroundOpacity), 0.0F, 1.0F);
    }
    return resolved;
  }

  WidgetBarCapsuleSpec
  resolvedTrayCapsuleSpec(const ConfigService& config, const BarConfig& bar, std::string_view widgetName) {
    return resolveWidgetBarCapsuleSpec(bar, trayWidgetConfig(config, widgetName));
  }

} // namespace tray
