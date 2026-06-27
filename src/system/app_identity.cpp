#include "system/app_identity.h"

#include "system/internal_app_metadata.h"
#include "util/string_utils.h"

#include <cctype>
#include <unordered_set>

namespace app_identity {

  namespace {

    std::string identityKey(std::string_view value) {
      std::string key;
      key.reserve(value.size());
      for (const unsigned char ch : value) {
        if (ch == '.' || ch == '-' || ch == '_' || std::isspace(ch) != 0) {
          continue;
        }
        key.push_back(static_cast<char>(std::tolower(ch)));
      }
      return key;
    }

    bool identityKeyMatches(std::string_view valueKey, std::string_view candidate) {
      if (candidate.empty()) {
        return false;
      }
      return valueKey == identityKey(candidate);
    }

    std::string_view appIdTail(std::string_view appKey) {
      std::string_view tail = appKey;
      if (const auto slash = tail.find_last_of('/'); slash != std::string_view::npos && slash + 1 < tail.size()) {
        tail = tail.substr(slash + 1);
      }
      if (const auto dot = tail.find_last_of('.'); dot != std::string_view::npos && dot + 1 < tail.size()) {
        tail = tail.substr(dot + 1);
      }
      return tail;
    }

    std::optional<DesktopEntry> findDesktopEntryByIdTail(
        std::string_view appKey, const std::vector<DesktopEntry>& allEntries, DesktopEntryLookupOptions options
    ) {
      const std::string appLower = StringUtils::toLower(std::string(appKey));
      const std::string tailLower = StringUtils::toLower(std::string(appIdTail(appKey)));
      if (tailLower.empty() || tailLower == appLower) {
        return std::nullopt;
      }

      std::vector<const DesktopEntry*> candidates;
      candidates.reserve(2);
      for (const auto& entry : allEntries) {
        if (!options.includeHidden && entry.hidden) {
          continue;
        }
        if (!options.includeNoDisplay && entry.noDisplay) {
          continue;
        }
        if (StringUtils::toLower(std::string(appIdTail(entry.id))) == tailLower) {
          candidates.push_back(&entry);
        }
      }
      if (candidates.empty()) {
        return std::nullopt;
      }
      if (candidates.size() == 1) {
        return *candidates.front();
      }

      const DesktopEntry* best = nullptr;
      for (const DesktopEntry* entry : candidates) {
        if (desktopEntryMatchesLower(*entry, appLower)) {
          if (best != nullptr) {
            return std::nullopt;
          }
          best = entry;
        }
      }
      if (best != nullptr) {
        return *best;
      }
      return std::nullopt;
    }

    struct DesktopEntryResolution {
      DesktopEntry entry;
      bool matchedDesktopEntry = false;
    };

    DesktopEntryResolution
    resolveRunningDesktopEntryWithStatus(std::string_view runningAppId, const std::vector<DesktopEntry>& allEntries) {
      const std::string runningLower = StringUtils::toLower(std::string(runningAppId));

      for (const auto& entry : allEntries) {
        if (entry.hidden || entry.noDisplay) {
          continue;
        }
        if (desktopEntryMatchesLower(entry, runningLower)) {
          return DesktopEntryResolution{
              .entry = entry,
              .matchedDesktopEntry = true,
          };
        }
      }

      DesktopEntryLookupOptions extendedLookup;
      if (runningAppId.starts_with("steam_app_")) {
        extendedLookup.includeHidden = true;
        extendedLookup.includeNoDisplay = true;
      }
      if (auto matched = findDesktopEntry(runningAppId, allEntries, extendedLookup)) {
        if (runningAppId.starts_with("steam_app_") && matched->startupWmClass.empty()) {
          matched->startupWmClass = std::string(runningAppId);
        }
        return DesktopEntryResolution{
            .entry = std::move(*matched),
            .matchedDesktopEntry = true,
        };
      }

      DesktopEntry fallback;
      fallback.id = std::string(runningAppId);
      fallback.name = std::string(runningAppId);
      fallback.nameLower = runningLower;
      internal_apps::applyMetadataToDesktopEntry(fallback);

      return DesktopEntryResolution{
          .entry = fallback,
          .matchedDesktopEntry = false,
      };
    }

  } // namespace

  bool matchesLower(
      std::string_view valueLower, std::string_view idLower, std::string_view startupWmClassLower,
      std::string_view nameLower
  ) {
    if (valueLower.empty()) {
      return false;
    }
    const auto valueKey = identityKey(valueLower);
    return valueLower == idLower
        || valueLower == startupWmClassLower
        || valueLower == nameLower
        || (!valueKey.empty()
            && (identityKeyMatches(valueKey, idLower) || identityKeyMatches(valueKey, startupWmClassLower)));
  }

  bool desktopEntryMatchesLower(const DesktopEntry& entry, std::string_view valueLower) {
    return matchesLower(
        valueLower, StringUtils::toLower(entry.id), StringUtils::toLower(entry.startupWmClass), entry.nameLower
    );
  }

  std::optional<DesktopEntry> findDesktopEntry(
      std::string_view appKey, const std::vector<DesktopEntry>& allEntries, DesktopEntryLookupOptions options
  ) {
    if (appKey.empty()) {
      return std::nullopt;
    }

    const std::string appLower = StringUtils::toLower(std::string(appKey));
    for (const auto& entry : allEntries) {
      if (!options.includeHidden && entry.hidden) {
        continue;
      }
      if (!options.includeNoDisplay && entry.noDisplay) {
        continue;
      }
      if (desktopEntryMatchesLower(entry, appLower)) {
        return entry;
      }
    }

    if (auto matched = findDesktopEntryByIdTail(appKey, allEntries, options)) {
      return matched;
    }

    if (!appKey.starts_with("steam_app_")) {
      return std::nullopt;
    }

    const std::string_view steamId = appKey.substr(std::string_view("steam_app_").size());
    if (steamId.empty()) {
      return std::nullopt;
    }
    const std::string runGameToken = std::string("rungameid/") + std::string(steamId);

    for (const auto& entry : allEntries) {
      if (!options.includeHidden && entry.hidden) {
        continue;
      }
      if (!options.includeNoDisplay && entry.noDisplay) {
        continue;
      }
      if (StringUtils::toLower(entry.startupWmClass) == appLower) {
        return entry;
      }
      if (entry.exec.contains(runGameToken)) {
        return entry;
      }
    }

    return std::nullopt;
  }

  DesktopEntry resolveRunningDesktopEntry(std::string_view runningAppId, const std::vector<DesktopEntry>& allEntries) {
    return resolveRunningDesktopEntryWithStatus(runningAppId, allEntries).entry;
  }

  std::vector<ResolvedRunningApp>
  resolveRunningApps(const std::vector<std::string>& runningAppIds, const std::vector<DesktopEntry>& allEntries) {
    std::vector<ResolvedRunningApp> resolved;
    resolved.reserve(runningAppIds.size());

    std::unordered_set<std::string> seen;
    seen.reserve(runningAppIds.size());

    for (const auto& runningAppId : runningAppIds) {
      const std::string runningLower = StringUtils::toLower(runningAppId);
      const auto resolution = resolveRunningDesktopEntryWithStatus(runningAppId, allEntries);
      std::string dedupeKey = resolution.matchedDesktopEntry ? StringUtils::toLower(resolution.entry.id) : runningLower;
      if (dedupeKey.empty()) {
        dedupeKey = runningLower;
      }
      if (!seen.insert(dedupeKey).second) {
        continue;
      }

      resolved.push_back(
          ResolvedRunningApp{
              .runningAppId = runningAppId,
              .runningLower = runningLower,
              .entry = resolution.entry,
          }
      );
    }

    return resolved;
  }

} // namespace app_identity
