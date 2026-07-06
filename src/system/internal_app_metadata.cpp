#include "system/internal_app_metadata.h"

#include "core/files/resource_paths.h"
#include "system/desktop_entry.h"
#include "util/string_utils.h"

namespace internal_apps {

  namespace {

    constexpr InternalAppDefinition kInternalApps[] = {
        {
            .appId = "dev.noctalia.Noctalia",
            .windowTitle = "Noctalia Settings",
            .displayName = "Noctalia",
            .iconAssetPath = "noctalia.svg",
        },
    };

  } // namespace

  const InternalAppDefinition* appDefinitionForAppId(std::string_view appId) {
    for (const auto& app : kInternalApps) {
      if (app.appId == appId) {
        return &app;
      }
    }
    return nullptr;
  }

  const InternalAppDefinition* appDefinitionForWindowTitle(std::string_view windowTitle) {
    for (const auto& app : kInternalApps) {
      if (!app.windowTitle.empty() && app.windowTitle == windowTitle) {
        return &app;
      }
    }
    return nullptr;
  }

  const InternalAppDefinition* definitionForDesktopEntry(const DesktopEntry& entry) {
    if (const auto* app = appDefinitionForAppId(entry.id)) {
      return app;
    }
    if (!entry.startupWmClass.empty()) {
      if (const auto* app = appDefinitionForAppId(entry.startupWmClass)) {
        return app;
      }
    }
    return appDefinitionForWindowTitle(entry.name);
  }

  [[nodiscard]] AppMetadata metadataFromDefinition(const InternalAppDefinition& app) {
    return AppMetadata{
        .displayName = std::string(app.displayName),
        .iconPath = paths::assetPath(app.iconAssetPath).string(),
    };
  }

  std::optional<AppMetadata> metadataForAppId(std::string_view appId) {
    const auto* app = appDefinitionForAppId(appId);
    if (app == nullptr) {
      return std::nullopt;
    }
    return metadataFromDefinition(*app);
  }

  std::optional<AppMetadata> metadataForDesktopEntry(const DesktopEntry& entry) {
    if (const auto* app = definitionForDesktopEntry(entry)) {
      return metadataForAppId(app->appId);
    }
    return std::nullopt;
  }

  void applyMetadataToDesktopEntry(DesktopEntry& entry) {
    const auto meta = metadataForDesktopEntry(entry);
    if (!meta.has_value()) {
      return;
    }
    if (entry.icon.empty()) {
      entry.icon = meta->iconPath;
    }
    if (entry.name == entry.id) {
      entry.name = meta->displayName;
      entry.nameLower = StringUtils::toLower(meta->displayName);
    }
  }

} // namespace internal_apps
