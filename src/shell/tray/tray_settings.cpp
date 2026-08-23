#include "shell/tray/tray_settings.h"

#include "config/config_service.h"

#include <string>
namespace {

  const WidgetConfig* canonicalTrayWidgetConfig(const ConfigService& config) {
    const auto& widgets = config.config().widgets;
    const auto configEntry = widgets.find(std::string(tray::kCanonicalTrayWidgetName));
    return configEntry != widgets.end() ? &configEntry->second : nullptr;
  }

} // namespace

namespace tray {

  ResolvedTrayOptions resolvedTrayOptions(const ConfigService& config, const TrayWidgetDefinitionContext& context) {
    const WidgetConfig* widgetConfig = canonicalTrayWidgetConfig(config);

    ResolvedTrayOptions resolved{
        .options = trayWidgetDefinition().resolve(widgetConfig, kCanonicalTrayWidgetName, context),
    };
    // The drawer distinguishes an omitted item size from the schema default. Keep
    // that single presence check beside the definition-owned resolution.
    if (widgetConfig != nullptr && widgetConfig->hasSetting("drawer_item_size")) {
      resolved.drawerItemSize = static_cast<float>(resolved.options.drawerItemSize);
    }
    return resolved;
  }

  WidgetBarCapsuleSpec resolvedTrayCapsuleSpec(const ConfigService& config, const BarConfig& bar) {
    return resolveWidgetBarCapsuleSpec(bar, canonicalTrayWidgetConfig(config));
  }

} // namespace tray
