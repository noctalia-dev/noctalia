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

  [[nodiscard]] const InternalAppDefinition* appDefinitionForAppId(std::string_view appId);
  [[nodiscard]] const InternalAppDefinition* appDefinitionForWindowTitle(std::string_view windowTitle);
  [[nodiscard]] const InternalAppDefinition* definitionForDesktopEntry(const DesktopEntry& entry);
  [[nodiscard]] std::optional<AppMetadata> metadataForAppId(std::string_view appId);
  [[nodiscard]] std::optional<AppMetadata> metadataForDesktopEntry(const DesktopEntry& entry);
  void applyMetadataToDesktopEntry(DesktopEntry& entry);

} // namespace internal_apps
