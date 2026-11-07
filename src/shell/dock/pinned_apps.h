#pragma once

#include "system/desktop_entry.h"

#include <string>
#include <string_view>
#include <vector>

namespace shell::dock::pinned_apps {

  bool matchesEntry(const DesktopEntry& entry, std::string_view pinnedId);
  bool containsEntry(const std::vector<std::string>& pinned, const DesktopEntry& entry);
  void removeEntry(std::vector<std::string>& pinned, const DesktopEntry& entry);
  std::vector<DesktopEntry> resolveEntries(const std::vector<std::string>& pinned);

} // namespace shell::dock::pinned_apps
