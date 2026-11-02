#pragma once

#include <optional>
#include <string>
#include <string_view>

struct DesktopEntry;

namespace internal_apps {

  struct AppMetadata {
    std::string displayName;
    std::string iconPath;
  };

  struct InternalAppDefinition {
    std::string_view appId;
    std::string_view windowTitle;
    std::string_view displayName;
    std::string_view iconAssetPath;
  };

  const InternalAppDefinition* appDefinitionForAppId(std::string_view appId);
  const InternalAppDefinition* appDefinitionForWindowTitle(std::string_view windowTitle);
  const InternalAppDefinition* definitionForDesktopEntry(const DesktopEntry& entry);
  std::optional<AppMetadata> metadataForAppId(std::string_view appId);
  std::optional<AppMetadata> metadataForDesktopEntry(const DesktopEntry& entry);
  void applyMetadataToDesktopEntry(DesktopEntry& entry);

} // namespace internal_apps
