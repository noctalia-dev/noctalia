#pragma once

#include "system/desktop_entry.h"

#include <string>
#include <string_view>
#include <vector>

namespace shell::dock::pinned_apps {

  // A `pinned` entry with this value draws a divider line instead of an app.
  inline constexpr std::string_view kSeparatorId = "separator";

  bool isSeparatorId(std::string_view pinnedId);
  // True for the entry resolveEntries() creates for a separator.
  bool isSeparator(const DesktopEntry& entry);

  // A `pinned` entry "stack:<folder>" shows that folder's newest files in a popup.
  inline constexpr std::string_view kStackPrefix = "stack:";

  bool isStackId(std::string_view pinnedId);
  // True for the entry resolveEntries() creates for a stack; its folder is in `workingDir`.
  bool isStack(const DesktopEntry& entry);

  bool matchesEntry(const DesktopEntry& entry, std::string_view pinnedId);
  bool containsEntry(const std::vector<std::string>& pinned, const DesktopEntry& entry);
  void removeEntry(std::vector<std::string>& pinned, const DesktopEntry& entry);
  std::vector<DesktopEntry> resolveEntries(const std::vector<std::string>& pinned);

} // namespace shell::dock::pinned_apps
