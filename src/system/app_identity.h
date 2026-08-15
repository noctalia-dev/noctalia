#pragma once

#include "system/desktop_entry.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace app_identity {

  struct ResolvedRunningApp {
    std::string runningAppId;
    std::string runningLower;
    DesktopEntry entry;
  };

  [[nodiscard]] bool matchesLower(
      std::string_view valueLower, std::string_view idLower, std::string_view startupWmClassLower,
      std::string_view nameLower
  );

  [[nodiscard]] bool desktopEntryMatchesLower(const DesktopEntry& entry, std::string_view valueLower);

  // Best-effort lookup by app id / StartupWMClass. Operates on the parsed desktop-entry list,
  // which already excludes hidden/NoDisplay/wrong-desktop entries.
  [[nodiscard]] std::optional<DesktopEntry> findDesktopEntry(
      std::string_view appKey, std::span<const DesktopEntry> allEntries,
      std::span<const DesktopEntry> priorityEntries = {}
  );

  [[nodiscard]] DesktopEntry
  resolveRunningDesktopEntry(std::string_view runningAppId, std::span<const DesktopEntry> allEntries);

  [[nodiscard]] std::vector<ResolvedRunningApp> resolveRunningApps(
      std::span<const std::string> runningAppIds, std::span<const DesktopEntry> allEntries,
      std::span<const DesktopEntry> priorityEntries = {}
  );

} // namespace app_identity
