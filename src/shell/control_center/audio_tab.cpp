#include "shell/control_center/audio_tab.h"

#include "config/config_service.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "pipewire/pipewire_service.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/control_center/tab.h"
#include "shell/panel/panel_manager.h"
#include "system/desktop_entry.h"
#include "system/icon_resolver.h"
#include "ui/builders.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/palette.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  constexpr float kValueLabelWidth = Style::controlHeightLg + Style::spaceLg;
  constexpr Logger kLogProgramUi{"audio_tab"};
  constexpr float kVolumeSyncEpsilon = 0.005f; // 0.5%
  constexpr auto kVolumeCommitInterval = std::chrono::milliseconds(16);
  constexpr auto kVolumeStateHoldoff = std::chrono::milliseconds(180);

  // Used to resolve application icons in AudioTab.
  IconResolver g_iconResolver;

  bool isGenericAudioLabel(std::string_view value) {
    if (value.empty()) {
      return true;
    }
    std::string normalized;
    normalized.reserve(value.size());
    for (const char ch : value) {
      if (ch == ' ' || ch == '_' || ch == '.') {
        normalized.push_back('-');
      } else {
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
      }
    }
    static constexpr std::string_view kBad[] = {"audio-src",   "audio-source", "audio-sink", "audio-output",
                                                "audio-input", "output",       "input",      "stream"};
    for (const auto token : kBad) {
      if (normalized == token) {
        return true;
      }
    }
    return false;
  }

  bool looksLikeRuntimeLauncher(std::string_view value) {
    const std::string normalized = StringUtils::toLower(value);
    if (normalized.empty()) {
      return false;
    }
    static constexpr std::string_view kRuntimeTokens[] = {
        "wine",   "wine64",       "wine64-preloader", "wineserver",
        "proton", "protontricks", "steam-runtime",    "pressure-vessel",
    };
    for (const auto token : kRuntimeTokens) {
      if (normalized == token || normalized.find(token) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  bool isLikelyFallbackStreamLabel(std::string_view value) {
    std::string normalized = StringUtils::toLower(value);
    for (char& ch : normalized) {
      if (std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == '_') {
        ch = '-';
      }
    }
    while (normalized.find("--") != std::string::npos) {
      normalized.erase(normalized.find("--"), 1);
    }
    return normalized.starts_with("audio-stream-") || normalized.starts_with("stream-") ||
           normalized.find("audio-stream-#") != std::string::npos;
  }

  bool isLowConfidenceProgramAppName(const AudioNode& node) {
    auto normalizeId = [](std::string value) {
      if (value.ends_with(".desktop")) {
        value.erase(value.size() - std::string_view(".desktop").size());
      }
      const auto lastSlash = value.find_last_of('/');
      if (lastSlash != std::string::npos) {
        value = value.substr(lastSlash + 1);
      }
      StringUtils::toLowerInPlace(value);
      return value;
    };

    if (node.applicationName.empty()) {
      return true;
    }
    if (isGenericAudioLabel(node.applicationName)) {
      return true;
    }

    const std::string appName = normalizeId(node.applicationName);
    const std::string appId = normalizeId(node.applicationId);
    const std::string appBinary = normalizeId(node.applicationBinary);
    const std::string nodeName = normalizeId(node.name);
    const std::string nodeDescription = normalizeId(node.description);
    if (appName.empty()) {
      return true;
    }
    if (!appId.empty() && appName == appId) {
      return false;
    }
    if (!appBinary.empty() && appName == appBinary) {
      return false;
    }
    if (!looksLikeRuntimeLauncher(appName) && (appName == nodeName || appName == nodeDescription)) {
      return false;
    }
    if (looksLikeRuntimeLauncher(appName)) {
      return true;
    }

    auto canonical = [](std::string value) {
      for (char& ch : value) {
        if (ch == '-' || ch == '_' || ch == '.' || std::isspace(static_cast<unsigned char>(ch)) != 0) {
          ch = ' ';
        }
      }
      // collapse spaces
      std::string out;
      out.reserve(value.size());
      bool prevSpace = true;
      for (const char ch : value) {
        const bool isSpace = std::isspace(static_cast<unsigned char>(ch)) != 0;
        if (isSpace) {
          if (!prevSpace) {
            out.push_back(' ');
          }
        } else {
          out.push_back(ch);
        }
        prevSpace = isSpace;
      }
      if (!out.empty() && out.back() == ' ') {
        out.pop_back();
      }
      return out;
    };

    const std::string canonicalName = canonical(appName);
    const std::string canonicalBinary = canonical(appBinary);
    const bool binaryMatchesName =
        !canonicalBinary.empty() &&
        (canonicalName == canonicalBinary || canonicalName.find(canonicalBinary) != std::string::npos ||
         canonicalBinary.find(canonicalName) != std::string::npos);
    // If we have no application.id and the binary disagrees with appName, appName is usually a runtime wrapper label.
    if (appId.empty() && !appBinary.empty() && !binaryMatchesName) {
      return true;
    }

    // Some stream clients expose a runtime/container name in application.name.
    // If application.id is more specific and does not match, prefer the id label.
    const bool idLooksSpecific = appId.find('.') != std::string::npos || appId.find('-') != std::string::npos ||
                                 appId.find('_') != std::string::npos;
    const bool nameLooksSimple = appName.find('.') == std::string::npos && appName.find('-') == std::string::npos &&
                                 appName.find('_') == std::string::npos && appName.find(' ') == std::string::npos;
    return !appId.empty() && appName != appId && idLooksSpecific && nameLooksSimple;
  }

  std::string prettifyIdentifier(std::string value) {
    if (value.empty()) {
      return value;
    }
    for (char& ch : value) {
      if (ch == '-' || ch == '_' || ch == '.') {
        ch = ' ';
      }
    }
    bool capitalize = true;
    for (char& ch : value) {
      if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
        capitalize = true;
        continue;
      }
      if (capitalize) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        capitalize = false;
      }
    }
    return value;
  }

  std::string lowerIdentifier(std::string value) {
    if (value.ends_with(".desktop")) {
      value.erase(value.size() - std::string_view(".desktop").size());
    }
    const auto lastSlash = value.find_last_of('/');
    if (lastSlash != std::string::npos) {
      value = value.substr(lastSlash + 1);
    }
    StringUtils::toLowerInPlace(value);
    return value;
  }

  [[nodiscard]] bool isBlankSearchKey(std::string_view s) { return StringUtils::isBlank(s); }

  bool isDesktopTokenDelimiter(unsigned char c) { return c == '-' || c == '_' || c == '.' || std::isspace(c) != 0; }

  // Substring match only when `needle` aligns with token boundaries in `haystack` (hyphen/dot/space/etc.).
  // Prevents "dark hours" matching KDE Ark via "ark" inside "d**ark**", and rejects accidental infixes.
  bool desktopSubstringMatchesWholeTokens(std::string_view haystack, std::string_view needle) {
    if (needle.empty() || haystack.empty() || needle.size() > haystack.size()) {
      return false;
    }
    for (std::size_t pos = 0; pos + needle.size() <= haystack.size();) {
      pos = haystack.find(needle, pos);
      if (pos == std::string_view::npos) {
        return false;
      }
      const bool leftOk = pos == 0 || isDesktopTokenDelimiter(static_cast<unsigned char>(haystack[pos - 1]));
      const std::size_t end = pos + needle.size();
      const bool rightOk = end == haystack.size() || isDesktopTokenDelimiter(static_cast<unsigned char>(haystack[end]));
      if (leftOk && rightOk) {
        return true;
      }
      pos += 1;
    }
    return false;
  }

  bool wholeTokenMatchAtStart(std::string_view haystack, std::string_view needle) {
    if (needle.empty() || haystack.size() < needle.size()) {
      return false;
    }
    if (haystack.substr(0, needle.size()) != needle) {
      return false;
    }
    const std::size_t end = needle.size();
    return end == haystack.size() || isDesktopTokenDelimiter(static_cast<unsigned char>(haystack[end]));
  }

  bool looksLikeSequelOnlySuffix(std::string_view s) {
    while (!s.empty() && isDesktopTokenDelimiter(static_cast<unsigned char>(s.front()))) {
      s.remove_prefix(1);
    }
    while (!s.empty() && isDesktopTokenDelimiter(static_cast<unsigned char>(s.back()))) {
      s.remove_suffix(1);
    }
    if (s.empty()) {
      return false;
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
      if (isDesktopTokenDelimiter(static_cast<unsigned char>(s[i]))) {
        return false;
      }
    }

    std::string tok = StringUtils::toLower(s);

    bool allDigit = true;
    for (char c : tok) {
      if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
        allDigit = false;
        break;
      }
    }
    if (allDigit) {
      return true;
    }

    static constexpr std::string_view kRoman[] = {"ii", "iii", "iv", "vi", "vii", "viii", "ix", "x", "xi", "xii"};
    for (const auto r : kRoman) {
      if (tok == r) {
        return true;
      }
    }

    // Spelled-out sequel ordinals (generic English product naming, not title-specific).
    static constexpr std::string_view kEnglishNumberWords[] = {"two",   "three", "four", "five",   "six",   "seven",
                                                               "eight", "nine",  "ten",  "eleven", "twelve"};
    for (const auto w : kEnglishNumberWords) {
      if (tok == w) {
        return true;
      }
    }
    return false;
  }

  bool crossMatchDesktopSearch(std::string_view lookupKey, std::string_view desktopField) {
    if (lookupKey.empty() || desktopField.empty()) {
      return false;
    }
    if (lookupKey == desktopField) {
      return true;
    }
    const std::string_view shorter = lookupKey.size() <= desktopField.size() ? lookupKey : desktopField;
    const std::string_view longer = lookupKey.size() <= desktopField.size() ? desktopField : lookupKey;
    if (shorter.size() < 2) {
      return false;
    }
    if (!desktopSubstringMatchesWholeTokens(longer, shorter)) {
      return false;
    }
    // Sequel/stream title is longer than base game's .desktop name ("… 2" vs "…"); do not pick the base entry.
    if (lookupKey.size() > desktopField.size() && wholeTokenMatchAtStart(lookupKey, desktopField)) {
      std::string_view remainder = lookupKey.substr(desktopField.size());
      while (!remainder.empty() && isDesktopTokenDelimiter(static_cast<unsigned char>(remainder.front()))) {
        remainder.remove_prefix(1);
      }
      if (looksLikeSequelOnlySuffix(remainder)) {
        return false;
      }
    }
    return true;
  }

  bool isValidDesktopMatch(std::string_view searchTerm, const DesktopEntry& entry) {
    if (searchTerm.empty()) {
      return false;
    }
    const std::string search = lowerIdentifier(std::string(searchTerm));
    if (isBlankSearchKey(search)) {
      return false;
    }
    const std::string id = lowerIdentifier(entry.id);
    const std::string name = lowerIdentifier(entry.name);
    const std::string icon = lowerIdentifier(entry.icon);
    return (!id.empty() && crossMatchDesktopSearch(search, id)) ||
           (!name.empty() && crossMatchDesktopSearch(search, name)) ||
           (!icon.empty() && crossMatchDesktopSearch(search, icon));
  }

  void pushUnique(std::vector<std::string>& values, std::string value) {
    if (value.empty()) {
      return;
    }
    if (std::ranges::find(values, value) == values.end()) {
      values.push_back(std::move(value));
    }
  }

  void appendMatchToken(std::vector<std::string>& values, std::string value) {
    value = lowerIdentifier(std::move(value));
    for (char& ch : value) {
      if (ch == ' ' || ch == '_') {
        ch = '-';
      }
    }
    pushUnique(values, value);

    const auto lastDot = value.find_last_of('.');
    if (lastDot != std::string::npos && lastDot + 1 < value.size()) {
      pushUnique(values, value.substr(lastDot + 1));
    }

    static constexpr std::string_view kMprisPrefix = "org.mpris.mediaplayer2.";
    if (value.starts_with(kMprisPrefix)) {
      pushUnique(values, value.substr(kMprisPrefix.size()));
    }

    static constexpr std::string_view kClientSuffix = "-client";
    if (value.ends_with(kClientSuffix) && value.size() > kClientSuffix.size()) {
      pushUnique(values, value.substr(0, value.size() - kClientSuffix.size()));
    }
  }

  std::vector<std::string> streamMatchTokens(const AudioNode& node, std::string_view resolvedAppName) {
    std::vector<std::string> tokens;
    appendMatchToken(tokens, node.applicationId);
    appendMatchToken(tokens, node.applicationBinary);
    appendMatchToken(tokens, node.applicationName);
    appendMatchToken(tokens, node.iconName);
    appendMatchToken(tokens, std::string(resolvedAppName));
    return tokens;
  }

  std::vector<std::string> playerMatchTokens(const MprisPlayerInfo& player) {
    std::vector<std::string> tokens;
    appendMatchToken(tokens, player.desktopEntry);
    appendMatchToken(tokens, player.identity);
    appendMatchToken(tokens, player.busName);
    return tokens;
  }

  const DesktopEntry* findDesktopEntryByTerm(std::string_view term) {
    const std::string trimmed = StringUtils::trim(term);
    if (trimmed.empty()) {
      return nullptr;
    }
    const std::string key = lowerIdentifier(trimmed);
    if (isBlankSearchKey(key)) {
      return nullptr;
    }
    for (const auto& entry : desktopEntries()) {
      if (isValidDesktopMatch(key, entry)) {
        return &entry;
      }
    }
    return nullptr;
  }

  struct DesktopEntryMatch {
    const DesktopEntry* entry = nullptr;
    const char* matchedVia = nullptr;
    std::string normalizedTerm;
  };

  DesktopEntryMatch lookupDesktopEntryForProgramStream(const AudioNode& node, std::string_view resolvedBeforeDesktop) {
    DesktopEntryMatch out;
    const std::string binary = lowerIdentifier(StringUtils::trim(node.applicationBinary));
    // Wine/Proton streams report wine64-preloader etc.; matching desktop entries by that binary (or
    // the shared Icon=wine) incorrectly picks unrelated apps (e.g. Protontricks) before app/node name.
    if (!binary.empty() && !looksLikeRuntimeLauncher(node.applicationBinary)) {
      if (const DesktopEntry* entry = findDesktopEntryByTerm(binary)) {
        out.entry = entry;
        out.matchedVia = "binary";
        out.normalizedTerm = binary;
        return out;
      }
    }
    const std::string appId = lowerIdentifier(StringUtils::trim(node.applicationId));
    if (!appId.empty()) {
      if (const DesktopEntry* entry = findDesktopEntryByTerm(appId)) {
        out.entry = entry;
        out.matchedVia = "application_id";
        out.normalizedTerm = appId;
        return out;
      }
    }
    const std::string appName = lowerIdentifier(StringUtils::trim(node.applicationName));
    if (!appName.empty() && !isGenericAudioLabel(appName) && !looksLikeRuntimeLauncher(appName)) {
      if (const DesktopEntry* entry = findDesktopEntryByTerm(appName)) {
        out.entry = entry;
        out.matchedVia = "application_name";
        out.normalizedTerm = appName;
        return out;
      }
    }
    const std::string resolved = lowerIdentifier(StringUtils::trim(resolvedBeforeDesktop));
    if (!resolved.empty() && !isGenericAudioLabel(resolved) && !looksLikeRuntimeLauncher(resolved)) {
      if (const DesktopEntry* entry = findDesktopEntryByTerm(resolved)) {
        out.entry = entry;
        out.matchedVia = "resolved_intermediate";
        out.normalizedTerm = resolved;
        return out;
      }
    }
    const std::string nodeName = lowerIdentifier(StringUtils::trim(node.name));
    if (!nodeName.empty()) {
      if (const DesktopEntry* entry = findDesktopEntryByTerm(nodeName)) {
        out.entry = entry;
        out.matchedVia = "node_name";
        out.normalizedTerm = nodeName;
        return out;
      }
    }
    return out;
  }

  const DesktopEntry* findDesktopEntryForNode(const AudioNode& node, std::string_view resolvedAppName) {
    return lookupDesktopEntryForProgramStream(node, resolvedAppName).entry;
  }

  struct ResolveProgramNameResult {
    std::string displayName;
    DesktopEntryMatch desktop;
  };

  ResolveProgramNameResult resolveProgramDisplayName(const AudioNode& node, const MprisPlayerInfo* player) {
    ResolveProgramNameResult result;
    std::string resolved = node.applicationName;
    const bool appNameIsGeneric = isLowConfidenceProgramAppName(node);

    if (appNameIsGeneric) {
      // Force fallback chain when app name is considered low-confidence.
      resolved.clear();
    }
    if (resolved.empty()) {
      if (!node.applicationId.empty() && !isGenericAudioLabel(node.applicationId)) {
        resolved = prettifyIdentifier(node.applicationId);
      }
    }
    if (resolved.empty() || isGenericAudioLabel(resolved)) {
      if (!node.applicationBinary.empty() && !isGenericAudioLabel(node.applicationBinary)) {
        resolved = prettifyIdentifier(node.applicationBinary);
      }
    }
    if ((resolved.empty() || isGenericAudioLabel(resolved) || looksLikeRuntimeLauncher(resolved)) &&
        !node.streamTitle.empty() && !isGenericAudioLabel(node.streamTitle) &&
        !looksLikeRuntimeLauncher(node.streamTitle) && !isLikelyFallbackStreamLabel(node.streamTitle)) {
      resolved = node.streamTitle;
    }
    if ((resolved.empty() || isGenericAudioLabel(resolved) || looksLikeRuntimeLauncher(resolved) ||
         (lowerIdentifier(resolved) == lowerIdentifier(node.name) &&
          lowerIdentifier(resolved) == lowerIdentifier(node.description) && node.applicationId.empty() &&
          node.applicationBinary.empty())) &&
        player != nullptr && !player->identity.empty() && !isGenericAudioLabel(player->identity)) {
      resolved = player->identity;
    }
    result.desktop = lookupDesktopEntryForProgramStream(node, resolved);
    if (result.desktop.entry != nullptr && !result.desktop.entry->name.empty() &&
        !isGenericAudioLabel(result.desktop.entry->name)) {
      resolved = result.desktop.entry->name;
    }
    if (resolved.empty() || isGenericAudioLabel(resolved) || looksLikeRuntimeLauncher(resolved)) {
      resolved = !node.name.empty() ? node.name : node.description;
    }
    if (isGenericAudioLabel(resolved) && !node.iconName.empty()) {
      resolved = prettifyIdentifier(node.iconName);
    }
    if ((resolved.empty() || isGenericAudioLabel(resolved)) && player != nullptr && !player->identity.empty()) {
      resolved = player->identity;
    }
    if (resolved.empty()) {
      resolved = "Application";
    }
    result.displayName = std::move(resolved);
    return result;
  }

  void logDesktopEntryMatchIfChanged(std::string& lastKey, std::uint32_t nodeId, const DesktopEntryMatch& desk) {
    if (desk.entry == nullptr || desk.matchedVia == nullptr) {
      return;
    }
    const std::string nextKey =
        std::to_string(nodeId) + "|" + desk.matchedVia + "|" + desk.normalizedTerm + "|" + desk.entry->id;
    if (nextKey == lastKey) {
      return;
    }
    lastKey = nextKey;
    kLogProgramUi.debug(
        "program stream desktop entry: pw.nodeId={} matched_via={} normalized_term='{}' entry.id='{}' "
        "entry.name='{}' entry.path='{}'",
        nodeId, desk.matchedVia, desk.normalizedTerm, desk.entry->id, desk.entry->name, desk.entry->path
    );
  }

  bool tokenListsMatch(const std::vector<std::string>& left, const std::vector<std::string>& right) {
    for (const auto& a : left) {
      if (a.empty() || isGenericAudioLabel(a)) {
        continue;
      }
      for (const auto& b : right) {
        if (b.empty() || isGenericAudioLabel(b)) {
          continue;
        }
        if (a == b) {
          return true;
        }
      }
    }
    return false;
  }

  std::string nowPlayingLabel(const MprisPlayerInfo* player) {
    if (player == nullptr || player->title.empty()) {
      return {};
    }
    const std::string artists = joinedArtists(player->artists);
    return artists.empty() ? player->title : artists + " - " + player->title;
  }

  std::string
  programResolutionIdentityKey(const AudioNode& node, const MprisPlayerInfo* player, std::string_view resolvedAppName) {
    std::string key;
    key.reserve(512);
    key = std::to_string(node.id);
    auto appendField = [&key](std::string_view field) {
      key.push_back('|');
      key.append(field);
    };
    appendField(node.applicationName);
    appendField(node.applicationId);
    appendField(node.applicationBinary);
    appendField(node.iconName);
    appendField(node.name);
    appendField(node.description);
    if (player != nullptr) {
      appendField(player->identity);
      appendField(player->busName);
      appendField(player->desktopEntry);
    }
    appendField(resolvedAppName);
    return key;
  }

  std::string formatTokenPreview(const std::vector<std::string>& tokens, std::size_t maxTokens) {
    std::string out;
    std::size_t shown = 0;
    for (const auto& t : tokens) {
      if (t.empty() || isGenericAudioLabel(t)) {
        continue;
      }
      if (shown >= maxTokens) {
        out += ", ...";
        break;
      }
      if (!out.empty()) {
        out += ", ";
      }
      out += t;
      ++shown;
    }
    return out;
  }

  void logProgramVolumeResolutionIfChanged(
      std::string& lastKey, const AudioNode& node, const MprisPlayerInfo* player, std::string_view resolvedAppName,
      std::size_t mprisPlayerCount
  ) {
    const std::string nextKey = programResolutionIdentityKey(node, player, resolvedAppName);
    if (nextKey == lastKey) {
      return;
    }
    lastKey = nextKey;
    const std::vector<std::string> streamTokens = streamMatchTokens(node, node.applicationName);
    const std::string tokenPreview = formatTokenPreview(streamTokens, 12);
    if (player != nullptr) {
      kLogProgramUi.debug(
          "application volume: id={} pw[app.name='{}' app.id='{}' binary='{}' icon='{}' node.name='{}' desc='{}'] "
          "mpris[match identity='{}' bus='{}' desktop='{}' status='{}'] resolved='{}' streamTokens=[{}] "
          "mprisPlayerCount={}",
          node.id, node.applicationName, node.applicationId, node.applicationBinary, node.iconName, node.name,
          node.description, player->identity, player->busName, player->desktopEntry, player->playbackStatus,
          resolvedAppName, tokenPreview, mprisPlayerCount
      );
    } else {
      kLogProgramUi.debug(
          "application volume: id={} pw[app.name='{}' app.id='{}' binary='{}' icon='{}' node.name='{}' desc='{}'] "
          "mpris[no match] resolved='{}' streamTokens=[{}] mprisPlayerCount={}",
          node.id, node.applicationName, node.applicationId, node.applicationBinary, node.iconName, node.name,
          node.description, resolvedAppName, tokenPreview, mprisPlayerCount
      );
    }
  }

  const MprisPlayerInfo* findMatchingPlayer(
      const std::vector<MprisPlayerInfo>& players, const AudioNode& node, std::string_view resolvedAppName
  ) {
    const std::vector<std::string> streamTokens = streamMatchTokens(node, resolvedAppName);
    const MprisPlayerInfo* fallback = nullptr;
    for (const auto& player : players) {
      if (!tokenListsMatch(streamTokens, playerMatchTokens(player))) {
        continue;
      }
      if (player.playbackStatus == "Playing") {
        return &player;
      }
      if (fallback == nullptr) {
        fallback = &player;
      }
    }
    return fallback;
  }

  std::vector<MprisPlayerInfo> allMprisPlayers(const MprisService* mpris) {
    if (mpris == nullptr) {
      return {};
    }
    std::vector<MprisPlayerInfo> players;
    const auto& cachedPlayers = mpris->players();
    players.reserve(cachedPlayers.size());
    for (const auto& [_, player] : cachedPlayers) {
      players.push_back(player);
    }
    return players;
  }

  void appendDesktopIconCandidates(
      std::vector<std::string>& candidates, const AudioNode& node, std::string_view resolvedAppName
  ) {
    if (const DesktopEntry* entry = findDesktopEntryForNode(node, resolvedAppName);
        entry != nullptr && !entry->icon.empty()) {
      pushUnique(candidates, entry->icon);
      pushUnique(candidates, entry->id);
    }
  }

  void appendFallbackIconCandidates(std::vector<std::string>& candidates, const AudioNode& node) {
    if (looksLikeRuntimeLauncher(node.applicationName) || looksLikeRuntimeLauncher(node.applicationBinary) ||
        looksLikeRuntimeLauncher(node.applicationId) || isLikelyFallbackStreamLabel(node.streamTitle)) {
      for (const std::string icon :
           {"wine", "steam", "applications-games", "application-x-executable", "application-default-icon"}) {
        pushUnique(candidates, icon);
      }
    }
  }

  class AudioDeviceRow : public Flex {
  public:
    explicit AudioDeviceRow(float scale, std::function<void()> onSelect) : m_onSelect(std::move(onSelect)) {
      setDirection(FlexDirection::Horizontal);
      setAlign(FlexAlign::Center);
      setGap(Style::spaceSm * scale);
      setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
      setMinHeight(Style::controlHeightLg * scale);
      setRadius(Style::scaledRadiusMd(scale));
      setFill(colorSpecFromRole(ColorRole::Surface));
      clearBorder();

      addChild(
          ui::radioButton({
              .out = &m_radio,
              .onChange = [this](bool) {
                if (m_onSelect) {
                  m_onSelect();
                }
              },
          })
      );

      addChild(
          ui::label({
              .out = &m_title,
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .fontWeight = FontWeight::Bold,
              .flexGrow = 1.0f,
          })
      );

      m_detail = nullptr;

      auto area = std::make_unique<InputArea>();
      area->setPropagateEvents(true);
      area->setOnEnter([this](const InputArea::PointerData&) { applyState(); });
      area->setOnLeave([this]() { applyState(); });
      area->setOnPress([this](const InputArea::PointerData&) { applyState(); });
      area->setOnClick([this](const InputArea::PointerData&) {
        if (m_onSelect) {
          m_onSelect();
        }
      });
      m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));

      applyState();
      m_paletteConn = paletteChanged().connect([this] { applyState(); });
    }

    void setDevice(const AudioNode& node) {
      m_radio->setChecked(node.isDefault);
      const std::string title = !node.description.empty() ? node.description : node.name;

      if (m_title != nullptr) {
        m_title->setText(title);
      }
    }

    void doLayout(Renderer& renderer) override {
      if (m_radio == nullptr || m_title == nullptr || m_inputArea == nullptr) {
        return;
      }

      m_radio->layout(renderer);

      const float textMaxWidth = std::max(0.0f, width() - paddingLeft() - paddingRight() - gap() - m_radio->width());
      m_title->setMaxWidth(textMaxWidth);

      m_inputArea->setVisible(false);
      Flex::doLayout(renderer);
      m_inputArea->setVisible(true);
      m_inputArea->setPosition(0.0f, 0.0f);
      m_inputArea->setSize(width(), height());

      applyState();
    }

    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
      return measureByLayout(renderer, constraints);
    }

    void doArrange(Renderer& renderer, const LayoutRect& rect) override { arrangeByLayout(renderer, rect); }

  private:
    void applyState() {
      if (pressed()) {
        setFill(colorSpecFromRole(ColorRole::Primary));
        setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
        if (m_title != nullptr) {
          m_title->setColor(colorSpecFromRole(ColorRole::OnPrimary));
        }
        return;
      }

      setFill(colorSpecFromRole(ColorRole::Surface));
      if (hovered()) {
        setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
      } else {
        clearBorder();
      }
      if (m_title != nullptr) {
        m_title->setColor(colorSpecFromRole(ColorRole::OnSurface));
      }
    }

    [[nodiscard]] bool hovered() const noexcept { return m_inputArea != nullptr && m_inputArea->hovered(); }
    [[nodiscard]] bool pressed() const noexcept { return m_inputArea != nullptr && m_inputArea->pressed(); }

    std::function<void()> m_onSelect;
    RadioButton* m_radio = nullptr;
    Label* m_title = nullptr;
    Label* m_detail = nullptr;
    InputArea* m_inputArea = nullptr;
    Signal<>::ScopedConnection m_paletteConn;
  };

  class ProgramVolumeRow : public Flex {
  public:
    ProgramVolumeRow(
        PipeWireService* audio, std::uint32_t id, float sliderMax, float scale,
        std::function<void(float)> onQueueVolume, std::function<void()> onCommitVolume
    )
        : m_audio(audio), m_id(id), m_sliderMax(sliderMax), m_onQueueVolume(std::move(onQueueVolume)),
          m_onCommitVolume(std::move(onCommitVolume)) {
      setDirection(FlexDirection::Vertical);
      setAlign(FlexAlign::Stretch);
      setPadding(Style::spaceXs * scale, Style::spaceMd * scale);
      setMinHeight((Style::controlHeightLg + Style::spaceXs) * scale);
      setRadius(Style::scaledRadiusMd(scale));
      setFill(colorSpecFromRole(ColorRole::Surface));
      clearBorder();

      constexpr float kIconSizeSm = 28.0f;
      m_iconSize = kIconSizeSm * scale;

      auto headerRow = ui::row({
          .out = &m_headerRow,
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * scale,
          .flexGrow = 0.0f,
      });

      headerRow->addChild(
          ui::image({
              .out = &m_icon,
              .fit = ImageFit::Contain,
              .radius = Style::scaledRadiusMd(scale),
              .width = m_iconSize,
              .height = m_iconSize,
              .visible = false,
          })
      );

      auto textCol = ui::column({
          .out = &m_textCol,
          .align = FlexAlign::Start,
          .justify = FlexJustify::Center,
          .gap = 0.0f,
          .flexGrow = 1.0f,
      });

      textCol->addChild(
          ui::label({
              .out = &m_appNameLabel,
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .fontWeight = FontWeight::Bold,
          })
      );

      textCol->addChild(
          ui::label({
              .out = &m_subtitleLabel,
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .visible = false,
              .configure = [](Label& label) { label.setCaptionStyle(); },
          })
      );

      headerRow->addChild(std::move(textCol));
      addChild(std::move(headerRow));

      auto controlsRow = ui::row({
          .out = &m_controlsRow,
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * scale,
          .flexGrow = 0.0f,
      });

      controlsRow->addChild(
          ui::slider({
              .out = &m_slider,
              .minValue = 0.0f,
              .maxValue = sliderMax,
              .step = 0.01f,
              .trackHeight = Style::sliderTrackHeight * scale,
              .thumbSize = Style::sliderThumbSize * scale,
              .controlHeight = Style::controlHeight * scale,
              .wheelAdjustEnabled = true,
              .flexGrow = 1.0f,
              .onValueChanged =
                  [this](float value) {
                    if (m_syncing || m_audio == nullptr) {
                      return;
                    }
                    if (m_valueLabel != nullptr) {
                      m_valueLabel->setText(std::to_string(static_cast<int>(std::round(value * 100.0f))) + "%");
                    }
                    if (m_onQueueVolume) {
                      m_onQueueVolume(value);
                    }
                  },
              .onDragEnd =
                  [this]() {
                    if (m_audio == nullptr) {
                      return;
                    }
                    if (m_onCommitVolume) {
                      m_onCommitVolume();
                    }
                  },
          })
      );

      controlsRow->addChild(
          ui::label({
              .out = &m_valueLabel,
              .text = "0%",
              .fontSize = Style::fontSizeBody * scale,
              .minWidth = kValueLabelWidth * scale,
              .fontWeight = FontWeight::Bold,
          })
      );

      controlsRow->addChild(
          ui::button({
              .out = &m_muteButton,
              .glyph = "volume-high",
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Default,
              .minWidth = Style::controlHeightSm * scale,
              .minHeight = Style::controlHeightSm * scale,
              .padding = Style::spaceXs * scale,
              .radius = Style::scaledRadiusMd(scale),
              .onClick = [this]() {
                if (m_audio == nullptr) {
                  return;
                }
                const bool nextMuted = !m_muted;
                m_audio->setProgramOutputMuted(m_id, nextMuted);
                PanelManager::instance().refresh();
              },
          })
      );
      addChild(std::move(controlsRow));
    }

    void doLayout(Renderer& renderer) override {
      if (m_icon != nullptr) {
        // Load icons lazily during layout.
        const bool iconIdentityChanged = m_lastLoadedIconIdentity != m_iconIdentityKey;
        if (iconIdentityChanged && !m_iconIdentityKey.empty() && !m_iconCandidates.empty()) {
          bool loaded = false;
          const int targetPx = static_cast<int>(std::round(m_iconSize));
          for (const std::string& candidate : m_iconCandidates) {
            const auto& resolved = g_iconResolver.resolve(candidate, targetPx);
            if (!resolved.empty()) {
              if (!m_lastIconPath.empty() && resolved == m_lastIconPath) {
                loaded = true;
                break;
              }
              loaded = m_icon->setSourceFile(renderer, resolved, targetPx, true);
              if (loaded) {
                m_lastIconPath = resolved;
                m_icon->setVisible(true);
              }
              break;
            }
          }
          if (!loaded) {
            m_icon->setVisible(false);
            m_lastIconPath.clear();
          }
          m_lastLoadedIconIdentity = m_iconIdentityKey;
        }
      }

      // Bound labels to protect row layout; only react when the row width actually changes.
      if (m_appNameLabel != nullptr) {
        const float rowWidth = width();
        if (std::abs(rowWidth - m_lastLabelLayoutWidth) >= 0.5f) {
          m_lastLabelLayoutWidth = rowWidth;
          const float textMax = std::max(80.0f, rowWidth - m_iconSize - gap() - paddingLeft() - paddingRight());
          m_appNameLabel->setMaxWidth(textMax);
          if (m_subtitleLabel != nullptr) {
            m_subtitleLabel->setMaxWidth(textMax);
          }
        }
      }

      Flex::doLayout(renderer);
    }

    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
      return measureByLayout(renderer, constraints);
    }

    void doArrange(Renderer& renderer, const LayoutRect& rect) override { arrangeByLayout(renderer, rect); }

    void syncFromNode(
        const AudioNode& node, const MprisPlayerInfo* player, bool isDefault, float sliderMax, bool nodeEnabled,
        std::size_t mprisPlayerCount
    ) {
      const ResolveProgramNameResult resolved = resolveProgramDisplayName(node, player);
      const std::string& resolvedAppName = resolved.displayName;
      logDesktopEntryMatchIfChanged(m_desktopMatchLogKey, node.id, resolved.desktop);
      logProgramVolumeResolutionIfChanged(m_programResolutionLogKey, node, player, resolvedAppName, mprisPlayerCount);

      std::string title = node.streamTitle;
      if (title.empty() && !node.name.empty() && node.name != resolvedAppName) {
        title = node.name;
      }
      if (title == resolvedAppName || isGenericAudioLabel(title) || isLikelyFallbackStreamLabel(title)) {
        title.clear();
      }
      if (title.empty()) {
        title = nowPlayingLabel(player);
      }

      const std::string appNameText = (isDefault ? "• " : "") + resolvedAppName;
      if (m_appNameLabel->text() != appNameText) {
        m_appNameLabel->setText(appNameText);
      }
      const bool showSubtitle = !title.empty() && title != resolvedAppName;
      if (m_subtitleLabel->visible() != showSubtitle) {
        m_subtitleLabel->setVisible(showSubtitle);
      }
      if (showSubtitle) {
        if (m_subtitleLabel->text() != title) {
          m_subtitleLabel->setText(title);
        }
      } else if (!m_subtitleLabel->text().empty()) {
        m_subtitleLabel->setText("");
      }

      const float clampedVolume = std::clamp(node.volume, 0.0f, sliderMax);
      const bool shouldSetSlider = nodeEnabled && m_slider != nullptr && !m_slider->dragging() && !m_syncing;
      if (m_slider != nullptr) {
        if (std::abs(m_sliderMax - sliderMax) >= 0.0001f) {
          m_sliderMax = sliderMax;
          m_slider->setRange(0.0f, sliderMax);
        }
        m_slider->setEnabled(nodeEnabled);
      }

      m_muted = node.muted;
      if (m_muteButton != nullptr) {
        m_muteButton->setEnabled(nodeEnabled);
        m_muteButton->setGlyph(m_muted ? "volume-mute" : "volume-high");
        m_muteButton->setVariant(m_muted ? ButtonVariant::Destructive : ButtonVariant::Default);
      }

      if (shouldSetSlider) {
        m_syncing = true;
        if (m_slider != nullptr) {
          m_slider->setValue(clampedVolume);
        }
        m_syncing = false;
        if (m_valueLabel != nullptr) {
          const std::string nextValue = std::to_string(static_cast<int>(std::round(clampedVolume * 100.0f))) + "%";
          if (m_valueLabel->text() != nextValue) {
            m_valueLabel->setText(nextValue);
          }
        }
      }

      std::vector<std::string> candidates;
      auto sanitize = [](std::string s) {
        const auto lastSlash = s.find_last_of('/');
        if (lastSlash != std::string::npos) {
          s = s.substr(lastSlash + 1);
        }
        if (s.ends_with(".desktop")) {
          s.erase(s.size() - std::string_view(".desktop").size());
        }

        for (std::string_view sep : {" - "}) {
          const auto pos = s.find(sep);
          if (pos != std::string::npos) {
            s = s.substr(0, pos);
            break;
          }
        }

        StringUtils::toLowerInPlace(s);
        for (char& c : s) {
          if (c == ' ' || c == '_') {
            c = '-';
          }
        }
        return s;
      };

      const std::string candidateIcon = sanitize(node.iconName);
      const std::string candidateId = sanitize(node.applicationId);
      const std::string candidateApp = sanitize(resolvedAppName);
      const std::string candidateFallback =
          sanitize(node.applicationBinary.empty() ? node.name : node.applicationBinary);
      if (!candidateApp.empty()) {
        pushUnique(candidates, candidateApp);
        pushUnique(candidates, candidateApp + ".desktop");
      }
      if (!candidateFallback.empty() && candidateFallback != candidateApp) {
        pushUnique(candidates, candidateFallback);
        pushUnique(candidates, candidateFallback + ".desktop");
      }
      if (!candidateId.empty() && candidateId != candidateApp && candidateId != candidateFallback) {
        pushUnique(candidates, candidateId);
        pushUnique(candidates, candidateId + ".desktop");
      }
      appendDesktopIconCandidates(candidates, node, resolvedAppName);
      appendFallbackIconCandidates(candidates, node);
      // Keep raw node icon as final fallback (Electron streams often report Chromium icon names).
      if (!candidateIcon.empty()) {
        pushUnique(candidates, candidateIcon);
        pushUnique(candidates, candidateIcon + ".desktop");
      }
      std::string nextIconIdentity;
      nextIconIdentity.reserve(128);
      for (const auto& candidate : candidates) {
        nextIconIdentity += candidate;
        nextIconIdentity.push_back('|');
      }
      if (nextIconIdentity != m_iconIdentityKey) {
        m_iconIdentityKey = std::move(nextIconIdentity);
        m_iconCandidates = std::move(candidates);
        m_lastLoadedIconIdentity.clear();
        m_lastIconPath.clear();
      }
    }

    void setValueLabelMinWidth(float minWidth) {
      if (m_valueLabel != nullptr) {
        m_valueLabel->setMinWidth(minWidth);
      }
    }

    [[nodiscard]] std::uint32_t id() const noexcept { return m_id; }
    [[nodiscard]] bool dragging() const noexcept { return m_slider != nullptr && m_slider->dragging(); }

  private:
    PipeWireService* m_audio = nullptr;
    std::uint32_t m_id = 0;

    Image* m_icon = nullptr;
    float m_iconSize = 0.0f;
    Flex* m_headerRow = nullptr;
    Flex* m_controlsRow = nullptr;
    Label* m_appNameLabel = nullptr;
    Label* m_subtitleLabel = nullptr;
    Flex* m_textCol = nullptr;

    std::string m_iconIdentityKey;
    std::string m_lastLoadedIconIdentity;
    std::string m_lastIconPath;
    std::vector<std::string> m_iconCandidates;
    float m_lastLabelLayoutWidth = -1.0f;
    std::string m_programResolutionLogKey;
    std::string m_desktopMatchLogKey;

    Slider* m_slider = nullptr;
    Label* m_valueLabel = nullptr;
    Button* m_muteButton = nullptr;

    bool m_syncing = false;
    bool m_muted = false;
    float m_sliderMax = 1.0f;

    std::function<void(float)> m_onQueueVolume;
    std::function<void()> m_onCommitVolume;
  };

  std::vector<AudioNode> sortedDevices(const std::vector<AudioNode>& devices) {
    std::vector<AudioNode> sorted = devices;
    std::ranges::sort(sorted, [](const AudioNode& a, const AudioNode& b) {
      const std::string& left = !a.description.empty() ? a.description : a.name;
      const std::string& right = !b.description.empty() ? b.description : b.name;
      if (left != right) {
        return left < right;
      }
      return a.id < b.id;
    });
    return sorted;
  }

  void addEmptyState(Flex& parent, const std::string& title, const std::string& body, float scale) {
    parent.addChild(
        ui::column(
            {
                .align = FlexAlign::Start,
                .gap = Style::spaceXs * scale,
                .padding = Style::spaceMd * scale,
                .configure =
                    [scale](Flex& card) {
                      card.setRadius(Style::scaledRadiusMd(scale));
                      card.setFill(colorSpecFromRole(ColorRole::Surface));
                      card.clearBorder();
                    },
            },
            ui::label({
                .text = title,
                .fontSize = Style::fontSizeBody * scale,
                .color = colorSpecFromRole(ColorRole::OnSurface),
                .fontWeight = FontWeight::Bold,
            }),
            ui::label({
                .text = body,
                .fontSize = Style::fontSizeCaption * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .configure = [](Label& label) { label.setCaptionStyle(); },
            })
        )
    );
  }

  std::string deviceListKey(const std::vector<AudioNode>& devices) {
    std::string key;
    for (const auto& device : devices) {
      key += std::to_string(device.id);
      key.push_back(':');
      key += device.isDefault ? '1' : '0';
      key.push_back(':');
      key += device.name;
      key.push_back(':');
      key += device.description;
      key.push_back('\n');
    }
    return key;
  }

  std::string widestPercentLabel(float sliderMaxValue) {
    const std::size_t digits =
        std::to_string(static_cast<int>(std::round(std::max(0.0f, sliderMaxValue) * 100.0f))).size();
    return std::string(std::max<std::size_t>(1, digits), '8') + "%";
  }

} // namespace

AudioTab::AudioTab(
    PipeWireService* audio, MprisService* mpris, ConfigService* config, WaylandConnection* wayland,
    RenderContext* renderContext
)
    : m_audio(audio), m_mpris(mpris), m_config(config), m_wayland(wayland), m_renderContext(renderContext) {}

AudioTab::~AudioTab() = default;

void AudioTab::openDeviceMenu(bool isOutput) {
  if (m_deviceMenuPopup == nullptr || m_audio == nullptr) {
    return;
  }

  m_deviceMenuIsOutput = isOutput;
  const AudioState& state = m_audio->state();
  const std::vector<AudioNode>& devices = isOutput ? state.sinks : state.sources;

  std::vector<ContextMenuControlEntry> entries;
  entries.reserve(devices.size());
  for (const auto& node : devices) {
    const std::uint32_t defaultId = isOutput ? state.defaultSinkId : state.defaultSourceId;
    const bool selected = node.id == defaultId;
    const std::string label = (selected ? "• " : "") + (!node.description.empty() ? node.description : node.name);
    entries.push_back(
        ContextMenuControlEntry{
            .id = static_cast<std::int32_t>(node.id),
            .label = label,
            .enabled = true,
            .separator = false,
            .hasSubmenu = false
        }
    );
  }

  Flex* anchor = isOutput ? m_outputDeviceMenuAnchor : m_inputDeviceMenuAnchor;
  if (anchor == nullptr) {
    return;
  }

  const auto parentCtx = PanelManager::instance().fallbackPopupParentContext();
  if (!parentCtx.has_value()) {
    return;
  }

  float anchorAbsX = 0.0f;
  float anchorAbsY = 0.0f;
  Node::absolutePosition(anchor, anchorAbsX, anchorAbsY);

  const float scale = contentScale();
  const float menuWidth = std::min(280.0f * scale, anchor->width());

  if (m_config != nullptr) {
    m_deviceMenuPopup->setShadowConfig(m_config->config().shell.shadow);
  }
  PanelManager::instance().beginAttachedPopup(parentCtx->surface);
  PanelManager::instance().setActivePopup(m_deviceMenuPopup.get());

  m_deviceMenuPopup->setOnDismissed([parentSurface = parentCtx->surface]() {
    PanelManager::instance().clearActivePopup();
    PanelManager::instance().endAttachedPopup(parentSurface);
  });

  m_deviceMenuPopup->open(
      std::move(entries), menuWidth, 10, static_cast<std::int32_t>(anchorAbsX), static_cast<std::int32_t>(anchorAbsY),
      static_cast<std::int32_t>(anchor->width()), static_cast<std::int32_t>(anchor->height()), parentCtx->layerSurface,
      parentCtx->output
  );
}

bool AudioTab::dragging() const noexcept {
  if ((m_outputSlider != nullptr && m_outputSlider->dragging()) ||
      (m_inputSlider != nullptr && m_inputSlider->dragging())) {
    return true;
  }
  for (Flex* row : m_programRows) {
    auto* programRow = static_cast<ProgramVolumeRow*>(row);
    if (programRow != nullptr && programRow->dragging()) {
      return true;
    }
  }
  return false;
}

bool AudioTab::dismissTransientUi() {
  if (m_deviceMenuPopup == nullptr || !m_deviceMenuPopup->isOpen()) {
    return false;
  }
  m_deviceMenuPopup->close();
  PanelManager::instance().clearActivePopup();
  return true;
}

std::unique_ptr<Flex> AudioTab::create() {
  const float scale = contentScale();
  const float sliderMax = sliderMaxPercent() / 100.0f;

  auto tab = ui::column({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  auto makeVolumeMenuButton = [this, scale](Button** out, bool output) {
    return ui::button({
        .out = out,
        .glyph = "more-vertical",
        .glyphSize = Style::fontSizeCaption * scale,
        .enabled = false,
        .variant = ButtonVariant::Ghost,
        .padding = Style::spaceXs * scale,
        .radius = Style::scaledRadiusMd(scale),
        .onClick = [this, output]() {
          const bool wasOpen = m_deviceMenuPopup != nullptr && m_deviceMenuPopup->isOpen();
          const bool wasOpenForThisDeviceType = wasOpen && m_deviceMenuIsOutput == output;
          if (wasOpen) {
            m_deviceMenuPopup->close();
            PanelManager::instance().clearActivePopup();
          }
          if (!wasOpenForThisDeviceType) {
            openDeviceMenu(output);
          }
        },
    });
  };

  auto makePercentLabel = [scale](Label** out) {
    return ui::label({
        .out = out,
        .text = "0%",
        .fontSize = Style::fontSizeBody * scale,
        .minWidth = kValueLabelWidth * scale,
        .fontWeight = FontWeight::Bold,
    });
  };

  auto volumeRow = ui::row({
      .out = &m_volumeColumn,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      // Keep volume cards at natural content height.
      .flexGrow = 0.0f,
  });

  auto outputVolumeCard = ui::column({
      .out = &m_outputVolumeCard,
      .flexGrow = 1.0f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applySectionCardStyle(card, scale, opacity, borders);
      },
  });

  auto outputHeader = ui::row({
      .out = &m_outputDeviceMenuAnchor,
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceXs * scale,
  });
  addTitle(*outputHeader, i18n::tr("control-center.audio.output-volume"), scale);
  outputHeader->addChild(makeVolumeMenuButton(&m_outputDeviceMenuButton, true));
  outputVolumeCard->addChild(std::move(outputHeader));
  outputVolumeCard->addChild(
      ui::label({
          .out = &m_outputDeviceLabel,
          .text = i18n::tr("control-center.audio.no-output-selected"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .configure = [](Label& label) { label.setCaptionStyle(); },
      })
  );

  auto outputRow = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
  });
  outputRow->addChild(
      ui::slider({
          .out = &m_outputSlider,
          .minValue = 0.0f,
          .maxValue = sliderMax,
          .step = 0.01f,
          .trackHeight = Style::sliderTrackHeight * scale,
          .thumbSize = Style::sliderThumbSize * scale,
          .controlHeight = Style::controlHeight * scale,
          .wheelAdjustEnabled = true,
          .flexGrow = 1.0f,
          .onValueChanged =
              [this](float value) {
                if (m_syncingOutputSlider || m_audio == nullptr) {
                  return;
                }
                m_sinkVolumeDebounceTimer.stop();
                queueSinkVolume(value);
                flushPendingVolumes();
                if (m_outputValue != nullptr) {
                  m_outputValue->setText(std::to_string(static_cast<int>(std::round(value * 100.0f))) + "%");
                }
              },
          .onDragEnd =
              [this]() {
                m_sinkVolumeDebounceTimer.stop();
                flushPendingVolumes();
              },
      })
  );
  outputRow->addChild(makePercentLabel(&m_outputValue));
  outputRow->addChild(
      ui::button({
          .out = &m_outputMuteButton,
          .glyph = "volume-high",
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Default,
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [this]() {
            if (m_audio == nullptr) {
              return;
            }
            if (const AudioNode* sink = m_audio->defaultSink(); sink != nullptr) {
              m_audio->setSinkMuted(sink->id, !sink->muted);
              PanelManager::instance().refresh();
            }
          },
      })
  );
  outputVolumeCard->addChild(std::move(outputRow));
  volumeRow->addChild(std::move(outputVolumeCard));

  auto inputVolumeCard = ui::column({
      .out = &m_inputVolumeCard,
      .flexGrow = 1.0f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applySectionCardStyle(card, scale, opacity, borders);
      },
  });

  auto inputHeader = ui::row({
      .out = &m_inputDeviceMenuAnchor,
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceXs * scale,
  });
  addTitle(*inputHeader, i18n::tr("control-center.audio.input-volume"), scale);
  inputHeader->addChild(makeVolumeMenuButton(&m_inputDeviceMenuButton, false));
  inputVolumeCard->addChild(std::move(inputHeader));
  inputVolumeCard->addChild(
      ui::label({
          .out = &m_inputDeviceLabel,
          .text = i18n::tr("control-center.audio.no-input-selected"),
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .configure = [](Label& label) { label.setCaptionStyle(); },
      })
  );

  auto inputRow = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
  });
  inputRow->addChild(
      ui::slider({
          .out = &m_inputSlider,
          .minValue = 0.0f,
          .maxValue = sliderMax,
          .step = 0.01f,
          .trackHeight = Style::sliderTrackHeight * scale,
          .thumbSize = Style::sliderThumbSize * scale,
          .controlHeight = Style::controlHeight * scale,
          .wheelAdjustEnabled = true,
          .flexGrow = 1.0f,
          .onValueChanged =
              [this](float value) {
                if (m_syncingInputSlider || m_audio == nullptr) {
                  return;
                }
                m_sourceVolumeDebounceTimer.stop();
                queueSourceVolume(value);
                flushPendingVolumes();
                if (m_inputValue != nullptr) {
                  m_inputValue->setText(std::to_string(static_cast<int>(std::round(value * 100.0f))) + "%");
                }
              },
          .onDragEnd =
              [this]() {
                m_sourceVolumeDebounceTimer.stop();
                flushPendingVolumes();
              },
      })
  );
  inputRow->addChild(makePercentLabel(&m_inputValue));
  inputRow->addChild(
      ui::button({
          .out = &m_inputMuteButton,
          .glyph = "microphone",
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Default,
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [this]() {
            if (m_audio == nullptr) {
              return;
            }
            if (const AudioNode* source = m_audio->defaultSource(); source != nullptr) {
              m_audio->setSourceMuted(source->id, !source->muted);
              PanelManager::instance().refresh();
            }
          },
      })
  );
  inputVolumeCard->addChild(std::move(inputRow));
  volumeRow->addChild(std::move(inputVolumeCard));

  tab->addChild(std::move(volumeRow));

  auto programCard = ui::column({
      .out = &m_programCard,
      .flexGrow = 1.0f,
      .configure = [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& card) {
        applySectionCardStyle(card, scale, opacity, borders);
      },
  });
  addTitle(*programCard, i18n::tr("control-center.audio.application-volumes"), scale);

  auto programScroll = ui::scrollView({
      .out = &m_programScroll,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0f,
      .viewportPaddingV = 0.0f,
      .flexGrow = 1.0f,
      .configure = [](ScrollView& scroll) {
        scroll.clearFill();
        scroll.clearBorder();
      },
  });

  m_programList = programScroll->content();
  m_programList->setDirection(FlexDirection::Vertical);
  m_programList->setAlign(FlexAlign::Stretch);
  m_programList->setGap(Style::spaceSm * scale);

  programCard->addChild(std::move(programScroll));
  tab->addChild(std::move(programCard));

  if (m_wayland != nullptr && m_renderContext != nullptr) {
    m_deviceMenuPopup = std::make_unique<ContextMenuPopup>(*m_wayland, *m_renderContext);
    m_deviceMenuPopup->setOnActivate([this](const ContextMenuControlEntry& entry) {
      if (m_audio == nullptr) {
        return;
      }
      const auto id = static_cast<std::uint32_t>(std::max<std::int32_t>(0, entry.id));
      if (m_deviceMenuIsOutput) {
        m_audio->setDefaultSink(id);
      } else {
        m_audio->setDefaultSource(id);
      }
    });
  }

  return tab;
}

void AudioTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_rootLayout == nullptr) {
    return;
  }

  syncValueLabelWidths(renderer);

  if (m_outputDeviceLabel != nullptr && m_outputVolumeCard != nullptr) {
    m_outputDeviceLabel->setMaxWidth(
        std::max(
            0.0f, m_outputVolumeCard->width() - m_outputVolumeCard->paddingLeft() - m_outputVolumeCard->paddingRight()
        )
    );
  }
  if (m_inputDeviceLabel != nullptr && m_inputVolumeCard != nullptr) {
    m_inputDeviceLabel->setMaxWidth(
        std::max(
            0.0f, m_inputVolumeCard->width() - m_inputVolumeCard->paddingLeft() - m_inputVolumeCard->paddingRight()
        )
    );
  }

  m_rootLayout->setSize(contentWidth, bodyHeight);
  m_rootLayout->layout(renderer);
}

void AudioTab::doUpdate(Renderer& renderer) {
  rebuildProgramVolumes(renderer);
  syncValueLabelWidths(renderer);

  if (m_audio != nullptr) {
    const AudioState& state = m_audio->state();

    if (m_outputDeviceMenuButton != nullptr) {
      const bool hasOutputs = !state.sinks.empty();
      m_outputDeviceMenuButton->setEnabled(hasOutputs);
      m_outputDeviceMenuButton->setVariant(hasOutputs ? ButtonVariant::Ghost : ButtonVariant::Default);
    }
    if (m_inputDeviceMenuButton != nullptr) {
      const bool hasInputs = !state.sources.empty();
      m_inputDeviceMenuButton->setEnabled(hasInputs);
      m_inputDeviceMenuButton->setVariant(hasInputs ? ButtonVariant::Ghost : ButtonVariant::Default);
    }
  }

  syncProgramVolumeRows();

  const float sliderMax = sliderMaxPercent() / 100.0f;
  if (m_outputSlider != nullptr) {
    m_syncingOutputSlider = true;
    m_outputSlider->setRange(0.0f, sliderMax);
    m_syncingOutputSlider = false;
  }
  if (m_inputSlider != nullptr) {
    m_syncingInputSlider = true;
    m_inputSlider->setRange(0.0f, sliderMax);
    m_syncingInputSlider = false;
  }

  const AudioNode* sink = m_audio != nullptr ? m_audio->defaultSink() : nullptr;
  const AudioNode* source = m_audio != nullptr ? m_audio->defaultSource() : nullptr;
  const auto now = std::chrono::steady_clock::now();
  const bool outputDragging = m_outputSlider != nullptr && m_outputSlider->dragging();
  const bool inputDragging = m_inputSlider != nullptr && m_inputSlider->dragging();

  if (m_outputDeviceLabel != nullptr) {
    m_outputDeviceLabel->setText(
        sink != nullptr ? (!sink->description.empty() ? sink->description : sink->name)
                        : i18n::tr("control-center.audio.no-output-selected")
    );
  }
  if (m_inputDeviceLabel != nullptr) {
    m_inputDeviceLabel->setText(
        source != nullptr ? (!source->description.empty() ? source->description : source->name)
                          : i18n::tr("control-center.audio.no-input-selected")
    );
  }

  const float sinkVolume = sink != nullptr ? sink->volume : 0.0f;
  const float sourceVolume = source != nullptr ? source->volume : 0.0f;
  const bool showPendingSink = sink != nullptr && m_pendingSinkVolume >= 0.0f && m_pendingSinkId == sink->id;
  const bool showPendingSource = source != nullptr && m_pendingSourceVolume >= 0.0f && m_pendingSourceId == source->id;
  const bool holdSinkState = outputDragging && sink != nullptr && m_lastSentSinkVolume >= 0.0f &&
                             now < m_ignoreSinkStateUntil && std::abs(sink->volume - m_lastSentSinkVolume) > 0.02f;
  const bool holdSourceState = inputDragging && source != nullptr && m_lastSentSourceVolume >= 0.0f &&
                               now < m_ignoreSourceStateUntil &&
                               std::abs(source->volume - m_lastSentSourceVolume) > 0.02f;
  const float displayedSinkVolume = std::clamp(
      showPendingSink ? m_pendingSinkVolume : (holdSinkState ? m_lastSentSinkVolume : sinkVolume), 0.0f, sliderMax
  );
  const float displayedSourceVolume = std::clamp(
      showPendingSource ? m_pendingSourceVolume : (holdSourceState ? m_lastSentSourceVolume : sourceVolume), 0.0f,
      sliderMax
  );

  if (m_outputSlider != nullptr) {
    m_outputSlider->setEnabled(sink != nullptr);
    if (!m_outputSlider->dragging() && std::abs(displayedSinkVolume - m_lastSinkVolume) >= kVolumeSyncEpsilon) {
      m_syncingOutputSlider = true;
      m_outputSlider->setValue(displayedSinkVolume);
      m_syncingOutputSlider = false;
      if (m_outputValue != nullptr) {
        m_outputValue->setText(std::to_string(static_cast<int>(std::round(displayedSinkVolume * 100.0f))) + "%");
      }
      m_lastSinkVolume = displayedSinkVolume;
    }
  }
  if (m_outputMuteButton != nullptr) {
    const bool outputMuted = sink != nullptr && sink->muted;
    m_outputMuteButton->setEnabled(sink != nullptr);
    m_outputMuteButton->setGlyph(outputMuted ? "volume-mute" : "volume-high");
    m_outputMuteButton->setVariant(outputMuted ? ButtonVariant::Destructive : ButtonVariant::Default);
  }

  if (m_inputSlider != nullptr) {
    m_inputSlider->setEnabled(source != nullptr);
    if (!m_inputSlider->dragging() && std::abs(displayedSourceVolume - m_lastSourceVolume) >= kVolumeSyncEpsilon) {
      m_syncingInputSlider = true;
      m_inputSlider->setValue(displayedSourceVolume);
      m_syncingInputSlider = false;
      if (m_inputValue != nullptr) {
        m_inputValue->setText(std::to_string(static_cast<int>(std::round(displayedSourceVolume * 100.0f))) + "%");
      }
      m_lastSourceVolume = displayedSourceVolume;
    }
  }
  if (m_inputMuteButton != nullptr) {
    const bool inputMuted = source != nullptr && source->muted;
    m_inputMuteButton->setEnabled(source != nullptr);
    m_inputMuteButton->setGlyph(inputMuted ? "microphone-mute" : "microphone");
    m_inputMuteButton->setVariant(inputMuted ? ButtonVariant::Destructive : ButtonVariant::Default);
  }
}

void AudioTab::onClose() {
  flushPendingVolumes(true);
  flushPendingProgramVolumes(true);
  m_sinkVolumeDebounceTimer.stop();
  m_sourceVolumeDebounceTimer.stop();
  m_programSinkDebounceTimer.stop();
  m_rootLayout = nullptr;
  m_deviceColumn = nullptr;
  m_outputCard = nullptr;
  m_inputCard = nullptr;
  m_outputScroll = nullptr;
  m_inputScroll = nullptr;
  m_outputList = nullptr;
  m_inputList = nullptr;
  m_volumeColumn = nullptr;
  m_outputVolumeCard = nullptr;
  m_inputVolumeCard = nullptr;
  m_outputDeviceLabel = nullptr;
  m_inputDeviceLabel = nullptr;
  m_outputSlider = nullptr;
  m_outputValue = nullptr;
  m_outputMuteButton = nullptr;
  m_inputSlider = nullptr;
  m_inputValue = nullptr;
  m_inputMuteButton = nullptr;
  m_lastOutputWidth = -1.0f;
  m_lastInputWidth = -1.0f;
  m_lastOutputListKey.clear();
  m_lastInputListKey.clear();
  m_programCard = nullptr;
  m_programScroll = nullptr;
  m_programList = nullptr;
  m_programRows.clear();
  m_lastProgramListKey.clear();
  m_lastProgramSliderMax = -1.0f;
  m_syncedPercentLabelMinWidth = -1.0f;
  m_lastSyncedPercentLabelSliderMax = -1.0f;
  m_outputDeviceMenuAnchor = nullptr;
  m_inputDeviceMenuAnchor = nullptr;
  m_outputDeviceMenuButton = nullptr;
  m_inputDeviceMenuButton = nullptr;
  if (m_deviceMenuPopup != nullptr) {
    PanelManager::instance().clearActivePopup();
    m_deviceMenuPopup->close();
  }
  m_pendingSinkId = 0;
  m_pendingSourceId = 0;
  m_lastSinkVolume = -1.0f;
  m_lastSourceVolume = -1.0f;
  m_pendingSinkVolume = -1.0f;
  m_pendingSourceVolume = -1.0f;
  m_pendingProgramSinkId = 0;
  m_pendingProgramSinkVolume = -1.0f;
  m_lastSentSinkVolume = -1.0f;
  m_lastSentSourceVolume = -1.0f;
  m_lastSinkCommitAt = {};
  m_lastSourceCommitAt = {};
  m_ignoreSinkStateUntil = {};
  m_ignoreSourceStateUntil = {};
}

void AudioTab::rebuildProgramVolumes(Renderer& renderer) {
  uiAssertNotRendering("AudioTab::rebuildProgramVolumes");
  if (m_programList == nullptr) {
    return;
  }

  const float scale = contentScale();
  const float sliderMax = sliderMaxPercent() / 100.0f;
  const float sliderMaxAbs = std::abs(sliderMax - m_lastProgramSliderMax);
  const std::vector<MprisPlayerInfo> players = allMprisPlayers(m_mpris);

  auto identityKey = [](const std::vector<AudioNode>& devices) -> std::string {
    std::string key;
    const auto sorted = sortedDevices(devices);
    key.reserve(sorted.size() * 48);
    for (const auto& node : sorted) {
      key += std::to_string(node.id);
      key.push_back(':');
      key += node.applicationId;
      key.push_back(':');
      key += node.applicationBinary;
      key.push_back(':');
      key += node.applicationName;
      key.push_back('\n');
    }
    return key;
  };

  const std::string nextKey =
      (m_audio != nullptr ? identityKey(m_audio->state().programOutputs) : std::string{"unavailable_program_outputs"});

  if (m_audio != nullptr && nextKey == m_lastProgramListKey && sliderMaxAbs < 0.0001f) {
    return;
  }

  while (!m_programList->children().empty()) {
    m_programList->removeChild(m_programList->children().front().get());
  }
  m_programRows.clear();

  if (m_audio == nullptr) {
    addEmptyState(
        *m_programList, i18n::tr("control-center.audio.unavailable-title"),
        i18n::tr("control-center.audio.unavailable-body"), scale
    );
    m_lastProgramListKey = nextKey;
    m_lastProgramSliderMax = sliderMax;
    return;
  }

  const AudioState& state = m_audio->state();

  if (state.programOutputs.empty()) {
    addEmptyState(
        *m_programList, i18n::tr("control-center.audio.no-application-audio"),
        i18n::tr("control-center.audio.no-application-audio-body"), scale
    );
  } else {
    for (const auto& sink : sortedDevices(state.programOutputs)) {
      const MprisPlayerInfo* player = findMatchingPlayer(players, sink, sink.applicationName);
      auto row = std::make_unique<ProgramVolumeRow>(
          m_audio, sink.id, sliderMax, scale,
          [this, sinkId = sink.id](float value) { queueProgramSinkVolume(sinkId, value); },
          [this]() { flushPendingProgramVolumes(true); }
      );
      row->syncFromNode(sink, player, false, sliderMax, true, players.size());
      m_programRows.push_back(row.get());
      m_programList->addChild(std::move(row));
    }
  }

  syncValueLabelWidths(renderer);
  m_programList->layout(renderer);
  m_lastProgramListKey = nextKey;
  m_lastProgramSliderMax = sliderMax;
}

void AudioTab::syncProgramVolumeRows() {
  if (m_audio == nullptr || m_programRows.empty()) {
    return;
  }

  const AudioState& state = m_audio->state();
  const float sliderMax = sliderMaxPercent() / 100.0f;
  const std::vector<MprisPlayerInfo> players = allMprisPlayers(m_mpris);

  std::unordered_map<std::uint32_t, const AudioNode*> outputsById;
  outputsById.reserve(state.programOutputs.size());
  for (const auto& s : state.programOutputs) {
    outputsById.emplace(s.id, &s);
  }

  for (Flex* node : m_programRows) {
    auto* row = static_cast<ProgramVolumeRow*>(node);
    if (row == nullptr) {
      continue;
    }
    const auto it = outputsById.find(row->id());
    if (it == outputsById.end()) {
      continue;
    }
    const MprisPlayerInfo* player = findMatchingPlayer(players, *it->second, it->second->applicationName);
    row->syncFromNode(*it->second, player, false, sliderMax, true, players.size());
  }
}

void AudioTab::queueProgramSinkVolume(std::uint32_t id, float value) {
  if (m_audio == nullptr || id == 0) {
    return;
  }

  const float sliderMax = sliderMaxPercent() / 100.0f;
  m_pendingProgramSinkId = id;
  m_pendingProgramSinkVolume = std::clamp(value, 0.0f, sliderMax);

  m_programSinkDebounceTimer.stop();
  m_programSinkDebounceTimer.start(kVolumeCommitInterval, [this]() { flushPendingProgramVolumes(false); });
}

void AudioTab::flushPendingProgramVolumes(bool) {
  if (m_audio == nullptr) {
    m_programSinkDebounceTimer.stop();
    m_pendingProgramSinkId = 0;
    m_pendingProgramSinkVolume = -1.0f;
    return;
  }

  const float sliderMax = sliderMaxPercent() / 100.0f;

  if (m_pendingProgramSinkVolume >= 0.0f && m_pendingProgramSinkId != 0) {
    const auto sinkId = m_pendingProgramSinkId;
    const float volume = std::clamp(m_pendingProgramSinkVolume, 0.0f, sliderMax);
    m_audio->setProgramOutputVolume(sinkId, volume);
  }
  m_programSinkDebounceTimer.stop();
  m_pendingProgramSinkId = 0;
  m_pendingProgramSinkVolume = -1.0f;
}

void AudioTab::rebuildLists(Renderer& renderer) {
  uiAssertNotRendering("AudioTab::rebuildLists");
  if (m_outputList == nullptr || m_inputList == nullptr || m_outputScroll == nullptr || m_inputScroll == nullptr) {
    return;
  }

  const float outputWidth = m_outputScroll->contentViewportWidth();
  const float inputWidth = m_inputScroll->contentViewportWidth();

  if (outputWidth <= 0.0f || inputWidth <= 0.0f) {
    return;
  }

  const float scale = contentScale();
  if (m_audio == nullptr) {
    if (outputWidth == m_lastOutputWidth && inputWidth == m_lastInputWidth && m_lastOutputListKey == "unavailable" &&
        m_lastInputListKey == "unavailable") {
      return;
    }
    while (!m_outputList->children().empty()) {
      m_outputList->removeChild(m_outputList->children().front().get());
    }
    while (!m_inputList->children().empty()) {
      m_inputList->removeChild(m_inputList->children().front().get());
    }
    addEmptyState(
        *m_outputList, i18n::tr("control-center.audio.unavailable-title"),
        i18n::tr("control-center.audio.unavailable-body"), scale
    );
    addEmptyState(
        *m_inputList, i18n::tr("control-center.audio.unavailable-title"),
        i18n::tr("control-center.audio.unavailable-body"), scale
    );
    m_lastOutputWidth = outputWidth;
    m_lastInputWidth = inputWidth;
    m_lastOutputListKey = "unavailable";
    m_lastInputListKey = "unavailable";
    return;
  }

  const AudioState& state = m_audio->state();
  const std::string nextOutputListKey = state.sinks.empty() ? "empty" : deviceListKey(state.sinks);
  const std::string nextInputListKey = state.sources.empty() ? "empty" : deviceListKey(state.sources);

  if (outputWidth == m_lastOutputWidth && inputWidth == m_lastInputWidth && nextOutputListKey == m_lastOutputListKey &&
      nextInputListKey == m_lastInputListKey) {
    return;
  }

  while (!m_outputList->children().empty()) {
    m_outputList->removeChild(m_outputList->children().front().get());
  }
  while (!m_inputList->children().empty()) {
    m_inputList->removeChild(m_inputList->children().front().get());
  }

  if (state.sinks.empty()) {
    addEmptyState(
        *m_outputList, i18n::tr("control-center.audio.no-output-devices"),
        i18n::tr("control-center.audio.no-output-devices-body"), scale
    );
  } else {
    for (const auto& sink : sortedDevices(state.sinks)) {
      auto row = std::make_unique<AudioDeviceRow>(scale, [this, id = sink.id]() {
        if (m_audio != nullptr) {
          m_audio->setDefaultSink(id);
        }
        PanelManager::instance().refresh();
      });
      row->setDevice(sink);
      m_outputList->addChild(std::move(row));
    }
  }

  if (state.sources.empty()) {
    addEmptyState(
        *m_inputList, i18n::tr("control-center.audio.no-input-devices"),
        i18n::tr("control-center.audio.no-input-devices-body"), scale
    );
  } else {
    for (const auto& source : sortedDevices(state.sources)) {
      auto row = std::make_unique<AudioDeviceRow>(scale, [this, id = source.id]() {
        if (m_audio != nullptr) {
          m_audio->setDefaultSource(id);
        }
        PanelManager::instance().refresh();
      });
      row->setDevice(source);
      m_inputList->addChild(std::move(row));
    }
  }

  m_outputList->layout(renderer);
  m_inputList->layout(renderer);

  m_lastOutputWidth = outputWidth;
  m_lastInputWidth = inputWidth;
  m_lastOutputListKey = nextOutputListKey;
  m_lastInputListKey = nextInputListKey;
}

void AudioTab::syncValueLabelWidths(Renderer& renderer) {
  const float sliderMax = sliderMaxPercent();
  if (m_syncedPercentLabelMinWidth < 0.0f || std::abs(sliderMax - m_lastSyncedPercentLabelSliderMax) >= 0.0001f) {
    const std::string sampleLabel = widestPercentLabel(sliderMax);
    const TextMetrics metrics =
        renderer.measureText(sampleLabel, Style::fontSizeBody * contentScale(), FontWeight::Bold);
    m_syncedPercentLabelMinWidth = std::round(metrics.width);
    m_lastSyncedPercentLabelSliderMax = sliderMax;
  }

  const float minWidth = m_syncedPercentLabelMinWidth;
  if (m_outputValue != nullptr) {
    m_outputValue->setMinWidth(minWidth);
  }
  if (m_inputValue != nullptr) {
    m_inputValue->setMinWidth(minWidth);
  }
  for (Flex* row : m_programRows) {
    if (auto* programRow = static_cast<ProgramVolumeRow*>(row); programRow != nullptr) {
      programRow->setValueLabelMinWidth(minWidth);
    }
  }
}

float AudioTab::sliderMaxPercent() const {
  return (m_config != nullptr && m_config->config().audio.enableOverdrive) ? 150.0f : 100.0f;
}

void AudioTab::queueSinkVolume(float value) {
  const AudioNode* sink = m_audio != nullptr ? m_audio->defaultSink() : nullptr;
  m_pendingSinkId = sink != nullptr ? sink->id : 0;
  m_pendingSinkVolume = std::clamp(value, 0.0f, sliderMaxPercent() / 100.0f);
}

void AudioTab::queueSourceVolume(float value) {
  const AudioNode* source = m_audio != nullptr ? m_audio->defaultSource() : nullptr;
  m_pendingSourceId = source != nullptr ? source->id : 0;
  m_pendingSourceVolume = std::clamp(value, 0.0f, sliderMaxPercent() / 100.0f);
}

void AudioTab::flushPendingVolumes(bool force) {
  if (m_audio == nullptr) {
    m_sinkVolumeDebounceTimer.stop();
    m_sourceVolumeDebounceTimer.stop();
    m_pendingSinkId = 0;
    m_pendingSourceId = 0;
    m_pendingSinkVolume = -1.0f;
    m_pendingSourceVolume = -1.0f;
    return;
  }

  const float sliderMax = sliderMaxPercent() / 100.0f;
  const bool outputDragging = m_outputSlider != nullptr && m_outputSlider->dragging();
  const bool inputDragging = m_inputSlider != nullptr && m_inputSlider->dragging();
  const auto now = std::chrono::steady_clock::now();

  if (m_pendingSinkVolume >= 0.0f) {
    m_pendingSinkVolume = std::clamp(m_pendingSinkVolume, 0.0f, sliderMax);
  }
  if (m_pendingSourceVolume >= 0.0f) {
    m_pendingSourceVolume = std::clamp(m_pendingSourceVolume, 0.0f, sliderMax);
  }

  if (m_pendingSinkVolume >= 0.0f) {
    const std::uint32_t sinkId = m_pendingSinkId;
    bool shouldSendSink = force;
    if (!shouldSendSink && sinkId != 0) {
      const float delta = std::abs(m_pendingSinkVolume - m_lastSentSinkVolume);
      shouldSendSink = delta >= 0.0001f;
    }
    if (shouldSendSink && !force && outputDragging) {
      const auto nextSendAt = m_lastSinkCommitAt + kVolumeCommitInterval;
      if (now < nextSendAt) {
        m_sinkVolumeDebounceTimer.start(
            std::chrono::duration_cast<std::chrono::milliseconds>(nextSendAt - now), [this]() { flushPendingVolumes(); }
        );
        shouldSendSink = false;
      }
    }
    if (sinkId != 0 && shouldSendSink) {
      m_audio->setSinkVolume(sinkId, m_pendingSinkVolume);
      m_audio->emitVolumePreview(false, sinkId, m_pendingSinkVolume);
      m_lastSentSinkVolume = m_pendingSinkVolume;
      m_lastSinkCommitAt = std::chrono::steady_clock::now();
      m_ignoreSinkStateUntil = m_lastSinkCommitAt + kVolumeStateHoldoff;
    }
    if (force || !outputDragging) {
      m_pendingSinkId = 0;
      m_pendingSinkVolume = -1.0f;
      m_sinkVolumeDebounceTimer.stop();
    }
  }

  if (m_pendingSourceVolume >= 0.0f) {
    const std::uint32_t sourceId = m_pendingSourceId;
    bool shouldSendSource = force;
    if (!shouldSendSource && sourceId != 0) {
      const float delta = std::abs(m_pendingSourceVolume - m_lastSentSourceVolume);
      shouldSendSource = delta >= 0.0001f;
    }
    if (shouldSendSource && !force && inputDragging) {
      const auto nextSendAt = m_lastSourceCommitAt + kVolumeCommitInterval;
      if (now < nextSendAt) {
        m_sourceVolumeDebounceTimer.start(
            std::chrono::duration_cast<std::chrono::milliseconds>(nextSendAt - now), [this]() { flushPendingVolumes(); }
        );
        shouldSendSource = false;
      }
    }
    if (sourceId != 0 && shouldSendSource) {
      m_audio->setSourceVolume(sourceId, m_pendingSourceVolume);
      m_audio->emitVolumePreview(true, sourceId, m_pendingSourceVolume);
      m_lastSentSourceVolume = m_pendingSourceVolume;
      m_lastSourceCommitAt = std::chrono::steady_clock::now();
      m_ignoreSourceStateUntil = m_lastSourceCommitAt + kVolumeStateHoldoff;
    }
    if (force || !inputDragging) {
      m_pendingSourceId = 0;
      m_pendingSourceVolume = -1.0f;
      m_sourceVolumeDebounceTimer.stop();
    }
  }
}
