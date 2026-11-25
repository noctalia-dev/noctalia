#include "shell/dock/pinned_apps.h"

#include "core/log.h"
#include "system/internal_app_metadata.h"

#include <algorithm>

namespace shell::dock::pinned_apps {
  namespace {

    constexpr Logger kLog("dock");

    DesktopEntry placeholderEntry(std::string_view pinnedId) {
      DesktopEntry placeholder;
      placeholder.id = std::string(pinnedId);
      placeholder.name = std::string(pinnedId);
      placeholder.nameLower = std::string(pinnedId);
      return placeholder;
    }

    DesktopEntry separatorEntry() {
      DesktopEntry separator;
      separator.id = std::string(kSeparatorId);
      separator.idLower = separator.id;
      return separator;
    }

  } // namespace

  bool isSeparatorId(std::string_view pinnedId) { return pinnedId == kSeparatorId; }

  // Desktop entries always have a path; the synthetic separator entry does not.
  bool isSeparator(const DesktopEntry& entry) { return entry.id == kSeparatorId && entry.path.empty(); }

  bool matchesEntry(const DesktopEntry& entry, std::string_view pinnedId) {
    if (pinnedId.empty()) {
      return false;
    }

    return entry.id == pinnedId;
  }

  bool containsEntry(const std::vector<std::string>& pinned, const DesktopEntry& entry) {
    return std::ranges::any_of(pinned, [&](const std::string& pinnedId) { return matchesEntry(entry, pinnedId); });
  }

  void removeEntry(std::vector<std::string>& pinned, const DesktopEntry& entry) {
    std::erase_if(pinned, [&](const std::string& pinnedId) { return matchesEntry(entry, pinnedId); });
  }

  std::vector<DesktopEntry> resolveEntries(const std::vector<std::string>& pinned) {
    std::vector<DesktopEntry> resolved;
    resolved.reserve(pinned.size());

    const auto& entries = desktopEntries();
    for (const auto& pinnedId : pinned) {
      if (isSeparatorId(pinnedId)) {
        resolved.push_back(separatorEntry());
        continue;
      }
      const auto match = std::ranges::find_if(entries, [&](const DesktopEntry& entry) {
        return !entry.hidden && !entry.noDisplay && entry.id == pinnedId;
      });

      DesktopEntry entry = match != entries.end() ? *match : [&] {
        kLog.debug("pinned app not found: {}", pinnedId);
        return placeholderEntry(pinnedId);
      }();
      internal_apps::applyMetadataToDesktopEntry(entry);
      resolved.push_back(std::move(entry));
    }

    return resolved;
  }

} // namespace shell::dock::pinned_apps
