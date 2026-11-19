#pragma once

#include "system/desktop_entry.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace shell::dock::pinned_apps {

  // A `pinned` entry with this value draws a divider line instead of an app.
  inline constexpr std::string_view kSeparatorId = "separator";

  bool isSeparatorId(std::string_view pinnedId);
  // True for the entry resolveEntries() creates for a separator.
  bool isSeparator(const DesktopEntry& entry);

  // Where running apps that are not pinned go; without it they follow the last pinned entry.
  inline constexpr std::string_view kRunningAppsId = "running_apps";

  bool isRunningAppsId(std::string_view pinnedId);
  // True for the placeholder entry resolveEntries() creates for "running_apps".
  bool isRunningApps(const DesktopEntry& entry);

  // A `pinned` entry "stack:<folder>" shows that folder's newest files in a popup.
  inline constexpr std::string_view kStackPrefix = "stack:";
  inline constexpr std::string_view kTrashId = "trash:";

  bool isStackId(std::string_view pinnedId);
  // A divider entry, and a stack entry for `folder`, as resolveEntries() creates them.
  DesktopEntry makeSeparatorEntry();
  DesktopEntry makeStackEntry(const std::filesystem::path& folder);
  // The built-in trash; its files folder is in `workingDir` and its icon shows whether it is empty.
  DesktopEntry makeTrashEntry();
  bool isTrash(const DesktopEntry& entry);
  // True for the entry resolveEntries() creates for a stack; its folder is in `workingDir`.
  bool isStack(const DesktopEntry& entry);

  bool matchesEntry(const DesktopEntry& entry, std::string_view pinnedId);
  bool containsEntry(const std::vector<std::string>& pinned, const DesktopEntry& entry);
  void removeEntry(std::vector<std::string>& pinned, const DesktopEntry& entry);
  std::vector<DesktopEntry> resolveEntries(const std::vector<std::string>& pinned);

} // namespace shell::dock::pinned_apps
