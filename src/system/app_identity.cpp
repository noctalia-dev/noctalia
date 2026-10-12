#include "system/app_identity.h"

#include "system/internal_app_metadata.h"
#include "util/string_utils.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <unordered_set>

namespace app_identity {

  namespace {

    enum class MatchStrength : std::uint8_t {
      None,
      Name,
      NormalizedStartupWmClass,
      NormalizedId,
      StartupWmClass,
      Id,
    };

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

    MatchStrength matchStrength(
        std::string_view valueLower, std::string_view idLower, std::string_view startupWmClassLower,
        std::string_view nameLower
    ) {
      if (valueLower.empty()) {
        return MatchStrength::None;
      }
      if (valueLower == idLower) {
        return MatchStrength::Id;
      }
      if (valueLower == startupWmClassLower) {
        return MatchStrength::StartupWmClass;
      }

      const std::string valueKey = identityKey(valueLower);
      if (!valueKey.empty() && identityKeyMatches(valueKey, idLower)) {
        return MatchStrength::NormalizedId;
      }
      if (!valueKey.empty() && identityKeyMatches(valueKey, startupWmClassLower)) {
        return MatchStrength::NormalizedStartupWmClass;
      }
      if (valueLower == nameLower) {
        return MatchStrength::Name;
      }
      return MatchStrength::None;
    }

    MatchStrength matchStrength(const DesktopEntry& entry, std::string_view valueLower) {
      const std::string idLower = entry.idLower.empty() ? StringUtils::toLower(entry.id) : entry.idLower;
      const std::string startupWmClassLower =
          entry.startupWmClassLower.empty() ? StringUtils::toLower(entry.startupWmClass) : entry.startupWmClassLower;
      const std::string nameLower = entry.nameLower.empty() ? StringUtils::toLower(entry.name) : entry.nameLower;
      return matchStrength(valueLower, idLower, startupWmClassLower, nameLower);
    }

    std::optional<DesktopEntry> findBestRankedMatch(std::string_view appLower, std::span<const DesktopEntry> entries) {
      const DesktopEntry* best = nullptr;
      MatchStrength bestStrength = MatchStrength::None;
      bool ambiguous = false;

      for (const auto& entry : entries) {
        const MatchStrength strength = matchStrength(entry, appLower);
        if (strength > bestStrength) {
          best = &entry;
          bestStrength = strength;
          ambiguous = false;
        } else if (
            strength != MatchStrength::None && strength == bestStrength && best != nullptr && entry.id != best->id
        ) {
          ambiguous = true;
        }
      }

      if (best == nullptr || ambiguous) {
        return std::nullopt;
      }
      return *best;
    }

    bool isGenericIdentityToken(std::string_view token) {
      static constexpr auto kGenericTokens = std::to_array<std::string_view>({
          "app",
          "application",
          "client",
          "desktop",
          "flatpak",
          "gui",
          "launcher",
          "linux",
          "wrapped",
      });
      return std::ranges::contains(kGenericTokens, token);
    }

    std::unordered_set<std::string> identityTokens(std::string_view value) {
      std::unordered_set<std::string> tokens;
      std::string token;
      token.reserve(value.size());

      auto flush = [&]() {
        if (token.size() >= 4U && !isGenericIdentityToken(token)) {
          tokens.insert(std::move(token));
        }
        token.clear();
      };

      for (const unsigned char ch : value) {
        if (std::isalnum(ch) != 0) {
          token.push_back(static_cast<char>(std::tolower(ch)));
        } else {
          flush();
        }
      }
      flush();
      return tokens;
    }

    std::size_t sharedIdentityTokenScore(const std::unordered_set<std::string>& appTokens, const DesktopEntry& entry) {
      auto entryTokens = identityTokens(entry.id);
      const auto startupWmClassTokens = identityTokens(entry.startupWmClass);
      entryTokens.insert(startupWmClassTokens.begin(), startupWmClassTokens.end());

      std::size_t score = 0;
      for (const auto& token : appTokens) {
        if (entryTokens.contains(token)) {
          score += token.size();
        }
      }
      return score;
    }

    std::optional<DesktopEntry>
    findDesktopEntryByUniqueTokens(std::string_view appKey, std::span<const DesktopEntry> allEntries) {
      const auto appTokens = identityTokens(appKey);
      if (appTokens.empty()) {
        return std::nullopt;
      }

      const DesktopEntry* best = nullptr;
      std::size_t bestScore = 0;
      bool ambiguous = false;
      for (const auto& entry : allEntries) {
        const std::size_t score = sharedIdentityTokenScore(appTokens, entry);
        if (score > bestScore) {
          best = &entry;
          bestScore = score;
          ambiguous = false;
        } else if (score != 0U && score == bestScore && best != nullptr && entry.id != best->id) {
          ambiguous = true;
        }
      }

      if (best == nullptr || ambiguous) {
        return std::nullopt;
      }
      return *best;
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

    std::optional<DesktopEntry>
    findDesktopEntryByIdTail(std::string_view appKey, std::span<const DesktopEntry> allEntries) {
      const std::string appLower = StringUtils::toLower(std::string(appKey));
      const std::string tailLower = StringUtils::toLower(std::string(appIdTail(appKey)));
      if (tailLower.empty() || tailLower == appLower) {
        return std::nullopt;
      }

      std::vector<const DesktopEntry*> candidates;
      candidates.reserve(2);
      for (const auto& entry : allEntries) {
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

    DesktopEntryResolution resolveRunningDesktopEntryWithStatus(
        std::string_view runningAppId, std::span<const DesktopEntry> allEntries,
        std::span<const DesktopEntry> priorityEntries = {}
    ) {
      if (auto matched = findDesktopEntry(runningAppId, allEntries, priorityEntries)) {
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
      fallback.nameLower = StringUtils::toLower(std::string(runningAppId));
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
    return matchStrength(valueLower, idLower, startupWmClassLower, nameLower) != MatchStrength::None;
  }

  bool desktopEntryMatchesLower(const DesktopEntry& entry, std::string_view valueLower) {
    return matchesLower(
        valueLower, StringUtils::toLower(entry.id), StringUtils::toLower(entry.startupWmClass), entry.nameLower
    );
  }

  std::optional<DesktopEntry> findDesktopEntry(
      std::string_view appKey, std::span<const DesktopEntry> allEntries, std::span<const DesktopEntry> priorityEntries
  ) {
    if (appKey.empty()) {
      return std::nullopt;
    }

    const std::string appLower = StringUtils::toLower(std::string(appKey));
    if (auto matched = findBestRankedMatch(appLower, priorityEntries)) {
      return matched;
    }
    if (auto matched = findBestRankedMatch(appLower, allEntries)) {
      return matched;
    }

    if (auto matched = findDesktopEntryByIdTail(appKey, allEntries)) {
      return matched;
    }

    if (!appKey.starts_with("steam_app_")) {
      return findDesktopEntryByUniqueTokens(appKey, allEntries);
    }

    const std::string_view steamId = appKey.substr(std::string_view("steam_app_").size());
    if (steamId.empty()) {
      return std::nullopt;
    }
    const std::string runGameToken = std::string("rungameid/") + std::string(steamId);

    for (const auto& entry : allEntries) {
      if (StringUtils::toLower(entry.startupWmClass) == appLower) {
        return entry;
      }
      if (entry.exec.contains(runGameToken)) {
        return entry;
      }
    }

    return std::nullopt;
  }

  DesktopEntry resolveRunningDesktopEntry(std::string_view runningAppId, std::span<const DesktopEntry> allEntries) {
    return resolveRunningDesktopEntryWithStatus(runningAppId, allEntries).entry;
  }

  std::vector<ResolvedRunningApp> resolveRunningApps(
      std::span<const std::string> runningAppIds, std::span<const DesktopEntry> allEntries,
      std::span<const DesktopEntry> priorityEntries
  ) {
    std::vector<ResolvedRunningApp> resolved;
    resolved.reserve(runningAppIds.size());

    std::unordered_set<std::string> seen;
    seen.reserve(runningAppIds.size());

    for (const auto& runningAppId : runningAppIds) {
      const std::string runningLower = StringUtils::toLower(runningAppId);
      const auto resolution = resolveRunningDesktopEntryWithStatus(runningAppId, allEntries, priorityEntries);
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
