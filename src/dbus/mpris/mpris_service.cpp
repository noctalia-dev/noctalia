#include "dbus/mpris/mpris_service.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "core/timer_manager.h"
#include "dbus/session_bus.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <sdbus-c++/IObject.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string_view>
#include <tuple>
#include <unordered_set>

std::string joinedArtists(const std::vector<std::string>& artists) {
  if (artists.empty()) {
    return {};
  }
  std::string joined = artists.front();
  for (std::size_t i = 1; i < artists.size(); ++i) {
    joined += ", ";
    joined += artists[i];
  }
  return joined;
}

namespace {

  constexpr auto kDbusInterface = "org.freedesktop.DBus";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
  constexpr auto kMprisRootInterface = "org.mpris.MediaPlayer2";
  constexpr auto kMprisPlayerInterface = "org.mpris.MediaPlayer2.Player";
  constexpr auto kNoctaliaMprisInterface = "dev.noctalia.Mpris";
  constexpr auto kPropertiesDebounceWindow = std::chrono::milliseconds{120};
  constexpr auto kMetadataStabilizeWindow = std::chrono::milliseconds{900};
  const sdbus::ServiceName kDbusName{"org.freedesktop.DBus"};
  const sdbus::ObjectPath kDbusPath{"/org/freedesktop/DBus"};
  const sdbus::ObjectPath kMprisPath{"/org/mpris/MediaPlayer2"};
  const sdbus::ServiceName kNoctaliaMprisBusName{"dev.noctalia.Mpris"};
  const sdbus::ObjectPath kNoctaliaMprisObjectPath{"/dev/noctalia/Mpris"};

  bool is_mpris_bus_name(std::string_view name) { return name.starts_with("org.mpris.MediaPlayer2."); }

  bool is_valid_loop_status(std::string_view loop_status) {
    return loop_status == "None" || loop_status == "Track" || loop_status == "Playlist";
  }

  std::string get_string_from_variant(const std::map<std::string, sdbus::Variant>& values, std::string_view key) {
    const auto it = values.find(std::string{key});
    if (it == values.end()) {
      return {};
    }

    try {
      return it->second.get<std::string>();
    } catch (const sdbus::Error&) {
      return {};
    }
  }

  std::string get_object_path_from_variant(const std::map<std::string, sdbus::Variant>& values, std::string_view key) {
    const auto it = values.find(std::string{key});
    if (it == values.end()) {
      return {};
    }

    try {
      return it->second.get<sdbus::ObjectPath>();
    } catch (const sdbus::Error&) {
      try {
        return it->second.get<std::string>();
      } catch (const sdbus::Error&) {
        return {};
      }
    }
  }

  std::vector<std::string>
  get_string_array_from_variant(const std::map<std::string, sdbus::Variant>& values, std::string_view key) {
    const auto it = values.find(std::string{key});
    if (it == values.end()) {
      return {};
    }

    try {
      return it->second.get<std::vector<std::string>>();
    } catch (const sdbus::Error&) {
      return {};
    }
  }

  int64_t get_int64_from_variant(const std::map<std::string, sdbus::Variant>& values, std::string_view key) {
    const auto it = values.find(std::string{key});
    if (it == values.end()) {
      return 0;
    }

    try {
      return it->second.get<int64_t>();
    } catch (const sdbus::Error&) {
      try {
        const auto value = it->second.get<uint64_t>();
        return value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ? std::numeric_limits<int64_t>::max()
                                                                                  : static_cast<int64_t>(value);
      } catch (const sdbus::Error&) {
        try {
          return static_cast<int64_t>(it->second.get<int32_t>());
        } catch (const sdbus::Error&) {
          try {
            return static_cast<int64_t>(it->second.get<uint32_t>());
          } catch (const sdbus::Error&) {
            return 0;
          }
        }
      }
    }
  }

  std::string get_string_from_props(const std::map<std::string, sdbus::Variant>& props, const char* key) {
    auto it = props.find(key);
    if (it == props.end()) {
      return {};
    }
    try {
      return it->second.get<std::string>();
    } catch (const sdbus::Error&) {
      return {};
    }
  }

  std::string
  get_string_from_props_or(const std::map<std::string, sdbus::Variant>& props, const char* key, const char* fallback) {
    auto it = props.find(key);
    if (it == props.end()) {
      return fallback;
    }
    try {
      return it->second.get<std::string>();
    } catch (const sdbus::Error&) {
      return fallback;
    }
  }

  bool get_bool_from_props(const std::map<std::string, sdbus::Variant>& props, const char* key) {
    auto it = props.find(key);
    if (it == props.end()) {
      return false;
    }
    try {
      return it->second.get<bool>();
    } catch (const sdbus::Error&) {
      return false;
    }
  }

  double get_double_from_props(const std::map<std::string, sdbus::Variant>& props, const char* key, double fallback) {
    auto it = props.find(key);
    if (it == props.end()) {
      return fallback;
    }
    try {
      return it->second.get<double>();
    } catch (const sdbus::Error&) {
      return fallback;
    }
  }

  std::map<std::string, sdbus::Variant>
  get_variant_map_from_props(const std::map<std::string, sdbus::Variant>& props, const char* key) {
    auto it = props.find(key);
    if (it == props.end()) {
      return {};
    }
    try {
      return it->second.get<std::map<std::string, sdbus::Variant>>();
    } catch (const sdbus::Error&) {
      return {};
    }
  }

  [[maybe_unused]] std::string primary_artist(const std::vector<std::string>& artists) {
    if (artists.empty()) {
      return {};
    }
    return artists.front();
  }

  [[maybe_unused]] std::string joinKeys(const std::map<std::string, sdbus::Variant>& values) {
    std::string out;
    bool first = true;
    for (const auto& [key, _] : values) {
      if (!first) {
        out += ", ";
      }
      first = false;
      out += key;
    }
    return out;
  }

  bool hasStrongNowPlayingMetadata(const MprisPlayerInfo& info) {
    // Track IDs/source URLs can exist during transient "loading" states where the
    // user-visible metadata is still placeholder-only (e.g. app identity + logo).
    // Treat metadata as strong only when actual now-playing fields are present.
    return !info.title.empty() || !info.artists.empty() || !info.album.empty();
  }

  std::vector<std::string> normalizeArtists(std::vector<std::string> artists) {
    std::erase_if(artists, [](const std::string& artist) { return StringUtils::trim(artist).empty(); });
    return artists;
  }

  std::string normalizeTrackId(std::string trackId) {
    if (trackId.ends_with("/NoTrack")) {
      return {};
    }
    return trackId;
  }

  std::string canonicalTrackSourceUrl(std::string_view rawUrl) {
    if (rawUrl.empty()) {
      return {};
    }

    std::string url(rawUrl);
    const auto fragmentPos = url.find('#');
    if (fragmentPos != std::string::npos) {
      url.resize(fragmentPos);
    }

    const auto queryPos = url.find('?');
    if (queryPos == std::string::npos) {
      return url;
    }

    const std::string base = url.substr(0, queryPos);
    const std::string query = url.substr(queryPos + 1);
    std::vector<std::string> keptParams;
    std::size_t start = 0;
    while (start <= query.size()) {
      const auto end = query.find('&', start);
      const std::string_view param = end == std::string::npos ? std::string_view(query).substr(start)
                                                              : std::string_view(query).substr(start, end - start);
      if (!param.empty()) {
        const auto equalsPos = param.find('=');
        const std::string_view key = equalsPos == std::string::npos ? param : param.substr(0, equalsPos);
        if (key != "t" && key != "start" && key != "time_continue") {
          keptParams.emplace_back(param);
        }
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }

    if (keptParams.empty()) {
      return base;
    }

    std::string normalized = base + "?";
    for (std::size_t i = 0; i < keptParams.size(); ++i) {
      if (i > 0) {
        normalized += '&';
      }
      normalized += keptParams[i];
    }
    return normalized;
  }

  std::string logicalTrackSignature(const MprisPlayerInfo& info) {
    const std::string canonicalSourceUrl = canonicalTrackSourceUrl(info.sourceUrl);
    if (!canonicalSourceUrl.empty()) {
      return std::format("{}\n{}", info.trackId, canonicalSourceUrl);
    }
    return std::format("{}\n{}\n{}\n{}", info.trackId, info.title, joinedArtists(info.artists), info.album);
  }

  std::map<std::string, sdbus::Variant> to_dbus_player(const MprisPlayerInfo& info) {
    std::map<std::string, sdbus::Variant> player;
    player["bus_name"] = sdbus::Variant(info.busName);
    player["identity"] = sdbus::Variant(info.identity);
    player["desktop_entry"] = sdbus::Variant(info.desktopEntry);
    player["playback_status"] = sdbus::Variant(info.playbackStatus);
    player["track_id"] = sdbus::Variant(info.trackId);
    player["title"] = sdbus::Variant(info.title);
    player["artists"] = sdbus::Variant(info.artists);
    player["album"] = sdbus::Variant(info.album);
    player["source_url"] = sdbus::Variant(info.sourceUrl);
    player["art_url"] = sdbus::Variant(info.artUrl);
    player["loop_status"] = sdbus::Variant(info.loopStatus);
    player["shuffle"] = sdbus::Variant(info.shuffle);
    player["volume"] = sdbus::Variant(info.volume);
    player["position_us"] = sdbus::Variant(info.positionUs);
    player["length_us"] = sdbus::Variant(info.lengthUs);
    player["can_play"] = sdbus::Variant(info.canPlay);
    player["can_pause"] = sdbus::Variant(info.canPause);
    player["can_go_next"] = sdbus::Variant(info.canGoNext);
    player["can_go_previous"] = sdbus::Variant(info.canGoPrevious);
    player["can_seek"] = sdbus::Variant(info.canSeek);
    return player;
  }

  // Some players report "infinite" stream duration as a sentinel near int64 max.
  // Treat those as unknown length (0) so UI duration/progress stays sane.
  int64_t sanitizeLengthUs(int64_t rawLengthUs) {
    if (rawLengthUs <= 0) {
      return 0;
    }
    constexpr int64_t kInfiniteLengthUsFloor = std::numeric_limits<int64_t>::max() / 1024;
    if (rawLengthUs >= kInfiniteLengthUsFloor) {
      return 0;
    }
    return rawLengthUs;
  }

  constexpr Logger kLog("mpris");
  constexpr auto kPositionRetryInterval = std::chrono::milliseconds{1000};
  constexpr auto kPositionCandidateRetryInterval = std::chrono::milliseconds{250};
  constexpr auto kPositionRetryInitialBackoff = std::chrono::milliseconds{2000};
  constexpr auto kPositionRetryMaxBackoff = std::chrono::milliseconds{30'000};
  // Threshold for consecutive failures while fetching full player properties in addOrRefreshPlayer.
  constexpr int kPlayerPropertiesFailureThreshold = 5;
  constexpr auto kRecentTrackChangeGuardWindow = std::chrono::milliseconds{8000};
  constexpr auto kRecentTrackChangeSlack = std::chrono::milliseconds{750};
  constexpr auto kPositionCandidateMatchWindow = std::chrono::milliseconds{2500};
  constexpr std::int64_t kPositionCandidateToleranceUs = 1500000;
  constexpr std::int64_t kPositionCandidateMinProgressUs = 250000;
  constexpr auto kSeekPauseGraceWindow = std::chrono::milliseconds{1500};
  constexpr std::int64_t kPausedSameTrackPositionJumpToleranceUs = 3000000;
  constexpr auto kNoSignalPauseRecoveryWindow = std::chrono::milliseconds{6000};
  constexpr std::int64_t kStaleRebaseClearSlackUs = 5000000;
  constexpr std::int64_t kPreviousTrackContinuationSlackUs = 15000000;
  constexpr std::int64_t kPauseRecoveryMinJumpUs = 10000000;
  constexpr std::int64_t kTrackLengthPositionSlackUs = 5000000;
  constexpr auto kInitialPositionProjectionGraceWindow = std::chrono::milliseconds{1250};
  constexpr std::int64_t kInitialPositionProjectionGraceCeilingUs = 5000000;

  bool isPlausibleTrackPosition(std::int64_t positionUs, std::int64_t lengthUs) {
    return lengthUs <= 0 || positionUs <= lengthUs + kTrackLengthPositionSlackUs;
  }

  std::string normalizeFilterToken(std::string_view value) { return StringUtils::toLower(StringUtils::trim(value)); }

} // namespace

MprisService::MprisService(SessionBus& bus)
    : m_bus(bus), m_dbusProxy(sdbus::createProxy(bus.connection(), kDbusName, kDbusPath)) {
  registerControlApi();
  registerBusSignals();
  discoverPlayers();
  scheduleStartupRediscovery();
}

const std::unordered_map<std::string, MprisPlayerInfo>& MprisService::players() const noexcept { return m_players; }

std::vector<MprisPlayerInfo> MprisService::listPlayers() const {
  std::vector<MprisPlayerInfo> result;
  result.reserve(m_players.size());
  for (const auto& [_, player] : m_players) {
    if (isBlacklisted(player)) {
      continue;
    }
    result.push_back(projectedPlayerInfo(player));
  }

  std::ranges::sort(result, [](const MprisPlayerInfo& a, const MprisPlayerInfo& b) { return a.busName < b.busName; });
  return result;
}

std::optional<MprisPlayerInfo> MprisService::activePlayer() const {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    return std::nullopt;
  }

  const auto it = m_players.find(*active);
  if (it == m_players.end()) {
    return std::nullopt;
  }
  return projectedPlayerInfo(it->second);
}

void MprisService::refreshPlayerPosition(const std::string& busName, bool notifyChange) {
  const auto proxyIt = m_playerProxies.find(busName);
  if (proxyIt == m_playerProxies.end() || !m_players.contains(busName)) {
    return;
  }

  const std::weak_ptr<void> aliveGuard = m_aliveGuard;
  try {
    proxyIt->second->callMethodAsync("Get")
        .onInterface(kPropertiesInterface)
        .withArguments(std::string{kMprisPlayerInterface}, std::string{"Position"})
        .uponReplyInvoke([this, aliveGuard, busName,
                          notifyChange](std::optional<sdbus::Error> err, sdbus::Variant value) {
          if (aliveGuard.expired()) {
            return;
          }
          if (err.has_value()) {
            kLog.warn("position refresh failed name={} err={}", busName, err->what());
            return;
          }
          const auto rawPositionUs = value.get<int64_t>();
          DeferredCall::callLater([this, aliveGuard, busName, notifyChange, rawPositionUs]() {
            if (aliveGuard.expired()) {
              return;
            }
            applyPositionSample(busName, rawPositionUs, notifyChange);
          });
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("position refresh dispatch failed name={} err={}", busName, e.what());
  }
}

bool MprisService::shouldRetryPropertiesRefresh(const std::string& busName) const {
  const auto failureIt = m_playerPropertiesFailures.find(busName);
  return failureIt == m_playerPropertiesFailures.end() || failureIt->second < kPlayerPropertiesFailureThreshold;
}

std::chrono::milliseconds MprisService::propertiesRefreshRetryInterval(
    const std::string& busName, std::chrono::milliseconds fallback, bool usePropertiesBackoff
) const {
  if (!usePropertiesBackoff) {
    return fallback;
  }
  if (const auto backoffIt = m_playerPropertiesRefreshBackoffMs.find(busName);
      backoffIt != m_playerPropertiesRefreshBackoffMs.end()) {
    return backoffIt->second;
  }
  return fallback;
}

void MprisService::schedulePositionRefreshRetry(
    const std::string& busName, std::chrono::milliseconds fallback, bool usePropertiesBackoff
) {
  auto& timerId = m_positionResyncTimers[busName];
  const std::weak_ptr<void> timerAliveGuard = m_aliveGuard;
  const auto retryInterval = propertiesRefreshRetryInterval(busName, fallback, usePropertiesBackoff);
  timerId = TimerManager::instance().start(timerId, retryInterval, [this, timerAliveGuard, busName]() {
    if (timerAliveGuard.expired()) {
      return;
    }
    refreshPlayerPosition(busName, true);
  });
}

void MprisService::applyPositionSample(const std::string& busName, int64_t rawPositionUs, bool notifyChange) {
  const auto playerIt = m_players.find(busName);
  if (playerIt == m_players.end()) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  const auto seekCommandIt = m_lastSeekCommandAt.find(busName);
  const bool recentLocalSeek =
      seekCommandIt != m_lastSeekCommandAt.end() && now - seekCommandIt->second <= kSeekPauseGraceWindow;
  auto offsetIt = m_positionOffsetsUs.find(busName);
  std::int64_t offsetUs = offsetIt != m_positionOffsetsUs.end() ? offsetIt->second : 0;
  std::int64_t normalizedUs = std::max<std::int64_t>(0, rawPositionUs - offsetUs);
  const bool hadAuthoritativeSample =
      m_hasAuthoritativePositionSample.contains(busName) && m_hasAuthoritativePositionSample.at(busName);
  const auto trackChangeIt = m_lastLogicalTrackChangeAt.find(busName);
  const bool guardingRecentTrackChange =
      trackChangeIt != m_lastLogicalTrackChangeAt.end() && now - trackChangeIt->second < kRecentTrackChangeGuardWindow;
  const std::int64_t elapsedSinceTrackChangeUs = trackChangeIt != m_lastLogicalTrackChangeAt.end()
      ? std::chrono::duration_cast<std::chrono::microseconds>(now - trackChangeIt->second).count()
      : 0;
  const std::int64_t maxPlausibleTrackPositionUs = elapsedSinceTrackChangeUs
      + std::chrono::duration_cast<std::chrono::microseconds>(kRecentTrackChangeSlack).count();
  const auto previousTrackRawIt = m_previousTrackRawPositionUs.find(busName);
  const bool hasPreviousTrackContext = previousTrackRawIt != m_previousTrackRawPositionUs.end();
  const bool looksLikePreviousTrackContinuation = hasPreviousTrackContext
      && std::llabs(rawPositionUs - previousTrackRawIt->second) <= kPreviousTrackContinuationSlackUs;

  if (offsetUs > 0 && !hasPreviousTrackContext && rawPositionUs + kStaleRebaseClearSlackUs < offsetUs) {
    offsetIt->second = 0;
    offsetUs = 0;
    normalizedUs = rawPositionUs;
  }

  if (!hadAuthoritativeSample
      && trackChangeIt != m_lastLogicalTrackChangeAt.end()
      && offsetUs > 0
      && rawPositionUs + kStaleRebaseClearSlackUs < offsetUs) {
    offsetIt->second = 0;
    offsetUs = 0;
    normalizedUs = rawPositionUs;
  }

  if (!hadAuthoritativeSample
      && trackChangeIt != m_lastLogicalTrackChangeAt.end()
      && playerIt->second.playbackStatus != "Stopped"
      && rawPositionUs > 5'000'000
      && normalizedUs > maxPlausibleTrackPositionUs
      && looksLikePreviousTrackContinuation) {
    offsetIt->second = rawPositionUs;
    if (playerIt->second.positionUs != 0) {
      playerIt->second.positionUs = 0;
      if (notifyChange && m_changeCallback) {
        m_changeCallback();
      }
    }
    if (playerIt->second.playbackStatus != "Stopped" && shouldRetryPropertiesRefresh(busName)) {
      schedulePositionRefreshRetry(busName, kPositionRetryInterval, true);
    }
    return;
  }

  bool authoritativeSample = false;
  if (normalizedUs > 0) {
    if (guardingRecentTrackChange) {
      authoritativeSample = normalizedUs <= maxPlausibleTrackPositionUs;
    } else {
      authoritativeSample = true;
    }
  } else if (playerIt->second.playbackStatus != "Playing") {
    authoritativeSample = hadAuthoritativeSample;
  }

  if (!authoritativeSample
      && !hadAuthoritativeSample
      && trackChangeIt != m_lastLogicalTrackChangeAt.end()
      && playerIt->second.playbackStatus != "Stopped"
      && normalizedUs > maxPlausibleTrackPositionUs
      && rawPositionUs > 5'000'000
      && hasPreviousTrackContext
      && !looksLikePreviousTrackContinuation) {
    authoritativeSample = true;
  }

  if (!m_pendingPositionSignalRefresh[busName]
      && normalizedUs == 0
      && playerIt->second.positionUs > 0
      && playerIt->second.playbackStatus != "Stopped") {
    return;
  }

  if (hadAuthoritativeSample && playerIt->second.playbackStatus == "Paused" && normalizedUs > 0) {
    const auto pauseIt = m_recentNoSignalPauseAt.find(busName);
    const bool recoveringRecentPause =
        pauseIt != m_recentNoSignalPauseAt.end() && now - pauseIt->second <= kNoSignalPauseRecoveryWindow;
    const std::int64_t pausedJumpUs = std::llabs(normalizedUs - playerIt->second.positionUs);
    if (recoveringRecentPause) {
      if (recentLocalSeek) {
        // A paused seek can legitimately jump without implying playback resumed.
      } else if (pausedJumpUs < kPauseRecoveryMinJumpUs) {
        return;
      } else {
        playerIt->second.playbackStatus = "Playing";
        m_recentNoSignalPauseAt.erase(pauseIt);
      }
    } else if (!recentLocalSeek && pausedJumpUs < kPausedSameTrackPositionJumpToleranceUs) {
      return;
    }
  }

  if (!hadAuthoritativeSample && !authoritativeSample && normalizedUs > 0) {
    const auto candidateIt = m_pendingPositionCandidateUs.find(busName);
    const auto candidateAtIt = m_pendingPositionCandidateAt.find(busName);
    const bool candidateFresh = candidateAtIt != m_pendingPositionCandidateAt.end()
        && now - candidateAtIt->second <= kPositionCandidateMatchWindow;
    bool candidateMatches = candidateIt != m_pendingPositionCandidateUs.end()
        && candidateFresh
        && std::llabs(candidateIt->second - normalizedUs) <= kPositionCandidateToleranceUs;

    if (candidateMatches && playerIt->second.playbackStatus == "Playing") {
      const auto elapsedSinceCandidateUs =
          std::chrono::duration_cast<std::chrono::microseconds>(now - candidateAtIt->second).count();
      const std::int64_t progressUs = normalizedUs - candidateIt->second;
      const std::int64_t maxExpectedProgressUs = elapsedSinceCandidateUs
          + std::chrono::duration_cast<std::chrono::microseconds>(kRecentTrackChangeSlack).count();
      candidateMatches = progressUs >= kPositionCandidateMinProgressUs && progressUs <= maxExpectedProgressUs;
    }

    if (!candidateMatches) {
      m_pendingPositionCandidateUs[busName] = normalizedUs;
      m_pendingPositionCandidateMatches[busName] = 0;
      m_pendingPositionCandidateAt[busName] = now;
      if (playerIt->second.playbackStatus == "Playing" && shouldRetryPropertiesRefresh(busName)) {
        schedulePositionRefreshRetry(busName, kPositionCandidateRetryInterval, false);
      }
      return;
    }

    if (guardingRecentTrackChange && playerIt->second.playbackStatus == "Playing") {
      int& matchCount = m_pendingPositionCandidateMatches[busName];
      ++matchCount;
      if (matchCount < 2) {
        m_pendingPositionCandidateUs[busName] = normalizedUs;
        m_pendingPositionCandidateAt[busName] = now;
        if (shouldRetryPropertiesRefresh(busName)) {
          schedulePositionRefreshRetry(busName, kPositionCandidateRetryInterval, false);
        }
        return;
      }
    }

    authoritativeSample = true;
  }

  if (!authoritativeSample) {
    const bool hasAuthoritativeSample =
        m_hasAuthoritativePositionSample.contains(busName) && m_hasAuthoritativePositionSample.at(busName);
    if (playerIt->second.playbackStatus == "Playing"
        && !hasAuthoritativeSample
        && shouldRetryPropertiesRefresh(busName)) {
      schedulePositionRefreshRetry(busName, kPositionRetryInterval, true);
    }
    return;
  }

  if (playerIt->second.positionUs != normalizedUs) {
    playerIt->second.positionUs = normalizedUs;
    if (notifyChange && m_changeCallback) {
      m_changeCallback();
    }
  }

  m_lastPositionSampleAt[busName] = now;
  m_hasAuthoritativePositionSample[busName] = true;
  m_pendingPositionCandidateUs.erase(busName);
  m_pendingPositionCandidateMatches.erase(busName);
  m_pendingPositionCandidateAt.erase(busName);
}

MprisPlayerInfo MprisService::projectedPlayerInfo(const MprisPlayerInfo& player) const {
  MprisPlayerInfo projected = player;
  projected.positionUs = projectedPositionUs(player);
  return projected;
}

std::int64_t MprisService::projectedPositionUs(const MprisPlayerInfo& player) const {
  if (player.playbackStatus == "Stopped") {
    return 0;
  }

  const bool hasAuthoritativeSample =
      m_hasAuthoritativePositionSample.contains(player.busName) && m_hasAuthoritativePositionSample.at(player.busName);
  std::int64_t projectedUs = std::max<std::int64_t>(0, player.positionUs);
  if (!hasAuthoritativeSample && player.playbackStatus == "Playing") {
    projectedUs = 0;
  }

  if (player.playbackStatus == "Playing" && hasAuthoritativeSample) {
    if (const auto it = m_lastPositionSampleAt.find(player.busName); it != m_lastPositionSampleAt.end()) {
      const auto trackChangeIt = m_lastLogicalTrackChangeAt.find(player.busName);
      const bool inInitialTrackWindow = trackChangeIt != m_lastLogicalTrackChangeAt.end()
          && std::chrono::steady_clock::now() - trackChangeIt->second <= kRecentTrackChangeGuardWindow;
      const bool suppressInitialProjection = inInitialTrackWindow
          && projectedUs <= kInitialPositionProjectionGraceCeilingUs
          && std::chrono::steady_clock::now() - it->second <= kInitialPositionProjectionGraceWindow;
      if (suppressInitialProjection) {
        if (player.lengthUs > 0) {
          return std::clamp<std::int64_t>(projectedUs, 0, player.lengthUs);
        }
        return projectedUs;
      }

      const auto elapsedUs =
          std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - it->second).count();
      if (elapsedUs > 0) {
        projectedUs += elapsedUs;
      }
    }
  }

  if (player.lengthUs > 0) {
    projectedUs = std::clamp<std::int64_t>(projectedUs, 0, player.lengthUs);
  }
  return projectedUs;
}

void MprisService::refreshPlayers() {
  kLog.debug("manual player refresh requested players_cached={}", m_players.size());
  discoverPlayers();
}

void MprisService::registerIpc(IpcService& ipc) {
  ipc.registerHandler(
      "media",
      [this](const std::string& args) -> std::string {
        const auto parts = noctalia::ipc::splitWords(args);
        if (parts.size() != 1) {
          return "error: media requires exactly one action <next|previous|toggle|stop>\n";
        }

        const std::string& action = parts[0];
        if (action == "next") {
          return nextActive() ? "ok\n" : "error: no active player or Next unsupported\n";
        }
        if (action == "previous") {
          return previousActive() ? "ok\n" : "error: no active player or Previous unsupported\n";
        }
        if (action == "toggle" || action == "playPause" || action == "play-pause") {
          return playPauseActive() ? "ok\n" : "error: no active player or PlayPause unsupported\n";
        }
        if (action == "stop") {
          return stopActive() ? "ok\n" : "error: no active player or Stop unsupported\n";
        }

        return "error: invalid media action (use next, previous, toggle, stop)\n";
      },
      "media <next|previous|toggle|stop>", "Control active media playback"
  );
}

std::function<void(std::optional<sdbus::Error>)>
MprisService::makeAsyncReplyHandler(std::string op, std::string busName) {
  const std::weak_ptr<void> aliveGuard = m_aliveGuard;
  return [this, aliveGuard, op = std::move(op), busName = std::move(busName)](std::optional<sdbus::Error> err) {
    if (aliveGuard.expired()) {
      return;
    }
    if (err.has_value()) {
      kLog.warn("{} failed name={} err={}", op, busName, err->what());
      return;
    }

    DeferredCall::callLater([this, aliveGuard, busName]() {
      if (aliveGuard.expired()) {
        return;
      }
      addOrRefreshPlayer(busName);
    });
  };
}

std::function<void(std::optional<sdbus::Error>)>
MprisService::makeAsyncReplyHandler(std::string op, std::string busName, std::string_view method) {
  const std::weak_ptr<void> aliveGuard = m_aliveGuard;
  return [this, aliveGuard, op = std::move(op), busName = std::move(busName),
          method = std::string(method)](std::optional<sdbus::Error> err) {
    if (aliveGuard.expired()) {
      return;
    }
    if (err.has_value()) {
      kLog.warn("{} failed name={} method={} err={}", op, busName, method, err->what());
      return;
    }

    kLog.debug("{} name={} method={}", op, busName, method);

    DeferredCall::callLater([this, aliveGuard, busName]() {
      if (aliveGuard.expired()) {
        return;
      }
      addOrRefreshPlayer(busName);
    });
  };
}

bool MprisService::playPause(const std::string& busName) {
  const auto it = m_players.find(busName);
  if (it == m_players.end()) {
    return false;
  }
  if (!canInvoke(it->second, "PlayPause")) {
    return false;
  }
  m_stoppedPlayers.erase(busName);
  return callPlayerMethod(busName, "PlayPause");
}

bool MprisService::stop(const std::string& busName) {
  const auto it = m_players.find(busName);
  if (it == m_players.end()) {
    return false;
  }
  if (!canInvoke(it->second, "Stop")) {
    return false;
  }
  if (!callPlayerMethod(busName, "Stop")) {
    return false;
  }
  dismissPlayer(busName);
  return true;
}

bool MprisService::next(const std::string& busName) {
  const auto it = m_players.find(busName);
  if (it == m_players.end()) {
    return false;
  }
  if (!canInvoke(it->second, "Next")) {
    return false;
  }
  return callPlayerMethod(busName, "Next");
}

bool MprisService::previous(const std::string& busName) {
  const auto it = m_players.find(busName);
  if (it == m_players.end()) {
    return false;
  }
  if (!canInvoke(it->second, "Previous")) {
    return false;
  }
  return callPlayerMethod(busName, "Previous");
}

bool MprisService::playPauseActive() {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    return false;
  }
  return playPause(*active);
}

bool MprisService::stopActive() {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    return false;
  }
  return stop(*active);
}

bool MprisService::nextActive() {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    return false;
  }
  return next(*active);
}

bool MprisService::previousActive() {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    return false;
  }
  return previous(*active);
}

bool MprisService::seek(const std::string& busName, int64_t offsetUs) {
  const auto it = m_players.find(busName);
  if (it == m_players.end() || !it->second.canSeek) {
    return false;
  }

  const auto proxyIt = m_playerProxies.find(busName);
  if (proxyIt == m_playerProxies.end()) {
    return false;
  }

  try {
    proxyIt->second->callMethodAsync("Seek")
        .onInterface(kMprisPlayerInterface)
        .withArguments(offsetUs)
        .uponReplyInvoke(makeAsyncReplyHandler("seek", busName));
    m_lastSeekCommandAt[busName] = std::chrono::steady_clock::now();
    return true;
  } catch (const sdbus::Error& e) {
    kLog.warn("seek dispatch failed name={} err={}", busName, e.what());
    return false;
  }
}

bool MprisService::seekActive(int64_t offsetUs) {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    return false;
  }
  return seek(*active, offsetUs);
}

bool MprisService::setPosition(const std::string& busName, int64_t positionUs) {
  const auto it = m_players.find(busName);
  if (it == m_players.end() || !it->second.canSeek) {
    return false;
  }

  const auto proxyIt = m_playerProxies.find(busName);
  if (proxyIt == m_playerProxies.end()) {
    return false;
  }

  // Use projected position to reduce stale-cache drift for relative-seek fallback.
  // Capture values by value, not iterator references.
  const int64_t currentPositionUs = projectedPositionUs(it->second);
  const bool preferRelativeSeek = it->second.trackId.empty() || busName.find("spotify") != std::string::npos;

  auto fallback_seek = [this, busName, currentPositionUs, positionUs]() {
    const int64_t offsetUs = positionUs - currentPositionUs;
    if (offsetUs == 0) {
      return true;
    }
    return seek(busName, offsetUs);
  };

  if (preferRelativeSeek) {
    // Some players don't expose track_id consistently; emulate absolute position with Seek.
    kLog.debug("mpris set-position using relative Seek fallback for {}", busName);
    return fallback_seek();
  }

  try {
    proxyIt->second->callMethodAsync("SetPosition")
        .onInterface(kMprisPlayerInterface)
        .withArguments(sdbus::ObjectPath{it->second.trackId}, positionUs)
        .uponReplyInvoke(makeAsyncReplyHandler("set-position", busName));
    m_lastSeekCommandAt[busName] = std::chrono::steady_clock::now();
    return true;
  } catch (const sdbus::Error& e) {
    kLog.warn("set-position dispatch failed name={} err={}, falling back to Seek", busName, e.what());
    return fallback_seek();
  }
}

bool MprisService::setPositionActive(int64_t positionUs) {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    return false;
  }
  return setPosition(*active, positionUs);
}

bool MprisService::setVolume(const std::string& busName, double volume) {
  if (!std::isfinite(volume) || volume < 0.0) {
    return false;
  }

  const auto it = m_players.find(busName);
  if (it == m_players.end()) {
    return false;
  }

  const auto proxyIt = m_playerProxies.find(busName);
  if (proxyIt == m_playerProxies.end()) {
    return false;
  }

  try {
    proxyIt->second->callMethodAsync("Set")
        .onInterface(kPropertiesInterface)
        .withArguments(std::string{kMprisPlayerInterface}, std::string{"Volume"}, sdbus::Variant{volume})
        .uponReplyInvoke(makeAsyncReplyHandler("set-volume", busName));
    return true;
  } catch (const sdbus::Error& e) {
    kLog.warn("set-volume dispatch failed name={} err={}", busName, e.what());
    return false;
  }
}

bool MprisService::setVolumeActive(double volume) {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    return false;
  }
  return setVolume(*active, volume);
}

bool MprisService::setShuffle(const std::string& busName, bool shuffle) {
  const auto it = m_players.find(busName);
  if (it == m_players.end()) {
    return false;
  }

  const auto proxyIt = m_playerProxies.find(busName);
  if (proxyIt == m_playerProxies.end()) {
    return false;
  }

  try {
    proxyIt->second->callMethodAsync("Set")
        .onInterface(kPropertiesInterface)
        .withArguments(std::string{kMprisPlayerInterface}, std::string{"Shuffle"}, sdbus::Variant{shuffle})
        .uponReplyInvoke(makeAsyncReplyHandler("set-shuffle", busName));
    return true;
  } catch (const sdbus::Error& e) {
    kLog.warn("set-shuffle dispatch failed name={} err={}", busName, e.what());
    return false;
  }
}

bool MprisService::setShuffleActive(bool shuffle) {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    return false;
  }
  return setShuffle(*active, shuffle);
}

bool MprisService::setLoopStatus(const std::string& busName, std::string loopStatus) {
  if (!is_valid_loop_status(loopStatus)) {
    return false;
  }

  const auto it = m_players.find(busName);
  if (it == m_players.end()) {
    return false;
  }

  const auto proxyIt = m_playerProxies.find(busName);
  if (proxyIt == m_playerProxies.end()) {
    return false;
  }

  try {
    proxyIt->second->callMethodAsync("Set")
        .onInterface(kPropertiesInterface)
        .withArguments(std::string{kMprisPlayerInterface}, std::string{"LoopStatus"}, sdbus::Variant{loopStatus})
        .uponReplyInvoke(makeAsyncReplyHandler("set-loop-status", busName));
    return true;
  } catch (const sdbus::Error& e) {
    kLog.warn("set-loop-status dispatch failed name={} err={}", busName, e.what());
    return false;
  }
}

bool MprisService::setLoopStatusActive(std::string loopStatus) {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    return false;
  }
  return setLoopStatus(*active, std::move(loopStatus));
}

std::optional<int64_t> MprisService::position(const std::string& busName) const {
  const auto it = m_players.find(busName);
  if (it == m_players.end()) {
    return std::nullopt;
  }
  return projectedPositionUs(it->second);
}

std::optional<int64_t> MprisService::positionActive() const {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    return std::nullopt;
  }
  return position(*active);
}

std::optional<double> MprisService::volume(const std::string& busName) const {
  const auto it = m_players.find(busName);
  if (it == m_players.end()) {
    return std::nullopt;
  }
  return it->second.volume;
}

std::optional<double> MprisService::volumeActive() const {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    return std::nullopt;
  }
  return volume(*active);
}

std::optional<bool> MprisService::shuffle(const std::string& busName) const {
  const auto it = m_players.find(busName);
  if (it == m_players.end()) {
    return std::nullopt;
  }
  return it->second.shuffle;
}

std::optional<bool> MprisService::shuffleActive() const {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    return std::nullopt;
  }
  return shuffle(*active);
}

std::optional<std::string> MprisService::loopStatus(const std::string& busName) const {
  const auto it = m_players.find(busName);
  if (it == m_players.end()) {
    return std::nullopt;
  }
  return it->second.loopStatus;
}

std::optional<std::string> MprisService::loopStatusActive() const {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    return std::nullopt;
  }
  return loopStatus(*active);
}

bool MprisService::setPinnedPlayerPreference(const std::string& busName) {
  const auto it = m_players.find(busName);
  if (it == m_players.end() || isBlacklisted(it->second)) {
    return false;
  }

  const auto previousActive = activePlayer();
  m_pinnedPlayerPreference = busName;
  syncSignals(previousActive);
  if (m_changeCallback) {
    m_changeCallback();
  }
  return true;
}

void MprisService::clearPinnedPlayerPreference() {
  const auto previousActive = activePlayer();
  m_pinnedPlayerPreference.reset();
  syncSignals(previousActive);
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void MprisService::setPreferredPlayers(std::vector<std::string> preferredBusNames) {
  std::unordered_set<std::string> seen;
  std::vector<std::string> normalized;
  normalized.reserve(preferredBusNames.size());

  for (auto& busName : preferredBusNames) {
    if (busName.empty()) {
      continue;
    }
    if (seen.insert(busName).second) {
      normalized.push_back(std::move(busName));
    }
  }

  const auto previousActive = activePlayer();
  m_preferredPlayers = std::move(normalized);
  syncSignals(previousActive);
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void MprisService::setBlacklist(std::vector<std::string> blacklist) {
  std::unordered_set<std::string> seen;
  std::vector<std::string> normalized;
  normalized.reserve(blacklist.size());

  for (const auto& raw : blacklist) {
    const std::string token = normalizeFilterToken(raw);
    if (token.empty()) {
      continue;
    }
    if (seen.insert(token).second) {
      normalized.push_back(token);
    }
  }

  if (m_blacklist == normalized) {
    return;
  }

  const auto previousActive = activePlayer();
  m_blacklist = std::move(normalized);

  if (m_pinnedPlayerPreference.has_value()) {
    const auto it = m_players.find(*m_pinnedPlayerPreference);
    if (it == m_players.end() || isBlacklisted(it->second)) {
      m_pinnedPlayerPreference.reset();
    }
  }

  emitPlayersChanged();
  syncSignals(previousActive);
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void MprisService::setChangeCallback(std::function<void()> callback) {
  m_changeCallback = std::move(callback);
  if (m_changeCallback && !m_players.empty()) {
    m_changeCallback();
  }
}

std::optional<std::string> MprisService::pinnedPlayerPreference() const { return m_pinnedPlayerPreference; }

const std::vector<std::string>& MprisService::preferredPlayers() const noexcept { return m_preferredPlayers; }

const std::vector<std::string>& MprisService::blacklist() const noexcept { return m_blacklist; }

void MprisService::registerControlApi() {
  m_bus.connection().requestName(kNoctaliaMprisBusName);
  m_controlObject = sdbus::createObject(m_bus.connection(), kNoctaliaMprisObjectPath);

  m_controlObject
      ->addVTable(
          sdbus::registerSignal("PlayersChanged")
              .withParameters<std::vector<std::map<std::string, sdbus::Variant>>>("players"),

          sdbus::registerSignal("ActivePlayerChanged")
              .withParameters<bool, std::map<std::string, sdbus::Variant>>("found", "player"),

          sdbus::registerSignal("TrackChanged")
              .withParameters<std::string, std::map<std::string, sdbus::Variant>>("player_bus_name", "player"),

          sdbus::registerMethod("GetPlayers").withOutputParamNames("players").implementedAs([this]() {
            std::vector<std::map<std::string, sdbus::Variant>> players;
            for (const auto& player : listPlayers()) {
              players.push_back(to_dbus_player(player));
            }
            return players;
          }),

          sdbus::registerMethod("GetActivePlayer").withOutputParamNames("found", "player").implementedAs([this]() {
            const auto active = activePlayer();
            if (!active.has_value()) {
              return std::make_tuple(false, std::map<std::string, sdbus::Variant>{});
            }
            return std::make_tuple(true, to_dbus_player(*active));
          }),

          sdbus::registerMethod("SetActivePlayerPreference")
              .withInputParamNames("player_bus_name")
              .withOutputParamNames("success")
              .implementedAs([this](const std::string& busName) { return onSetActivePlayerPreference(busName); }),

          sdbus::registerMethod("ClearActivePlayerPreference").withOutputParamNames("success").implementedAs([this]() {
            return onClearActivePlayerPreference();
          }),

          sdbus::registerMethod("SetPreferredPlayers")
              .withInputParamNames("preferred_bus_names")
              .withOutputParamNames("success")
              .implementedAs([this](const std::vector<std::string>& preferredBusNames) {
                return onSetPreferredPlayers(preferredBusNames);
              }),

          sdbus::registerMethod("GetPlayerPreferences")
              .withOutputParamNames("has_pinned", "pinned_bus_name", "preferred_bus_names")
              .implementedAs([this]() { return onGetPlayerPreferences(); }),

          sdbus::registerMethod("GetPositionPlayer")
              .withInputParamNames("player_bus_name")
              .withOutputParamNames("position_us")
              .implementedAs([this](const std::string& busName) { return onGetPositionPlayer(busName); }),

          sdbus::registerMethod("GetPositionActive").withOutputParamNames("position_us").implementedAs([this]() {
            return onGetPositionActive();
          }),

          sdbus::registerMethod("GetVolumePlayer")
              .withInputParamNames("player_bus_name")
              .withOutputParamNames("volume")
              .implementedAs([this](const std::string& busName) { return onGetVolumePlayer(busName); }),

          sdbus::registerMethod("GetVolumeActive").withOutputParamNames("volume").implementedAs([this]() {
            return onGetVolumeActive();
          }),

          sdbus::registerMethod("SetVolumePlayer")
              .withInputParamNames("player_bus_name", "volume")
              .withOutputParamNames("success")
              .implementedAs([this](const std::string& busName, double volume) {
                return onSetVolumePlayer(busName, volume);
              }),

          sdbus::registerMethod("SetVolumeActive")
              .withInputParamNames("volume")
              .withOutputParamNames("success")
              .implementedAs([this](double volume) { return onSetVolumeActive(volume); }),

          sdbus::registerMethod("GetShufflePlayer")
              .withInputParamNames("player_bus_name")
              .withOutputParamNames("shuffle")
              .implementedAs([this](const std::string& busName) { return onGetShufflePlayer(busName); }),

          sdbus::registerMethod("GetShuffleActive").withOutputParamNames("shuffle").implementedAs([this]() {
            return onGetShuffleActive();
          }),

          sdbus::registerMethod("SetShufflePlayer")
              .withInputParamNames("player_bus_name", "shuffle")
              .withOutputParamNames("success")
              .implementedAs([this](const std::string& busName, bool shuffle) {
                return onSetShufflePlayer(busName, shuffle);
              }),

          sdbus::registerMethod("SetShuffleActive")
              .withInputParamNames("shuffle")
              .withOutputParamNames("success")
              .implementedAs([this](bool shuffle) { return onSetShuffleActive(shuffle); }),

          sdbus::registerMethod("GetLoopStatusPlayer")
              .withInputParamNames("player_bus_name")
              .withOutputParamNames("loop_status")
              .implementedAs([this](const std::string& busName) { return onGetLoopStatusPlayer(busName); }),

          sdbus::registerMethod("GetLoopStatusActive").withOutputParamNames("loop_status").implementedAs([this]() {
            return onGetLoopStatusActive();
          }),

          sdbus::registerMethod("SetLoopStatusPlayer")
              .withInputParamNames("player_bus_name", "loop_status")
              .withOutputParamNames("success")
              .implementedAs([this](const std::string& busName, const std::string& loopStatus) {
                return onSetLoopStatusPlayer(busName, loopStatus);
              }),

          sdbus::registerMethod("SetLoopStatusActive")
              .withInputParamNames("loop_status")
              .withOutputParamNames("success")
              .implementedAs([this](const std::string& loopStatus) { return onSetLoopStatusActive(loopStatus); }),

          sdbus::registerMethod("SeekPlayer")
              .withInputParamNames("player_bus_name", "offset_us")
              .withOutputParamNames("success")
              .implementedAs([this](const std::string& busName, int64_t offsetUs) {
                return onSeekPlayer(busName, offsetUs);
              }),

          sdbus::registerMethod("SeekActive")
              .withInputParamNames("offset_us")
              .withOutputParamNames("success")
              .implementedAs([this](int64_t offsetUs) { return onSeekActive(offsetUs); }),

          sdbus::registerMethod("SetPositionPlayer")
              .withInputParamNames("player_bus_name", "position_us")
              .withOutputParamNames("success")
              .implementedAs([this](const std::string& busName, int64_t positionUs) {
                return onSetPositionPlayer(busName, positionUs);
              }),

          sdbus::registerMethod("SetPositionActive")
              .withInputParamNames("position_us")
              .withOutputParamNames("success")
              .implementedAs([this](int64_t positionUs) { return onSetPositionActive(positionUs); }),

          sdbus::registerMethod("PlayPausePlayer")
              .withInputParamNames("player_bus_name")
              .withOutputParamNames("success")
              .implementedAs([this](const std::string& busName) { return onPlayPausePlayer(busName); }),

          sdbus::registerMethod("StopPlayer")
              .withInputParamNames("player_bus_name")
              .withOutputParamNames("success")
              .implementedAs([this](const std::string& busName) { return onStopPlayer(busName); }),

          sdbus::registerMethod("NextPlayer")
              .withInputParamNames("player_bus_name")
              .withOutputParamNames("success")
              .implementedAs([this](const std::string& busName) { return onNextPlayer(busName); }),

          sdbus::registerMethod("PreviousPlayer")
              .withInputParamNames("player_bus_name")
              .withOutputParamNames("success")
              .implementedAs([this](const std::string& busName) { return onPreviousPlayer(busName); }),

          sdbus::registerMethod("PlayPauseActive").withOutputParamNames("success").implementedAs([this]() {
            return onPlayPauseActive();
          }),

          sdbus::registerMethod("StopActive").withOutputParamNames("success").implementedAs([this]() {
            return onStopActive();
          }),

          sdbus::registerMethod("NextActive").withOutputParamNames("success").implementedAs([this]() {
            return onNextActive();
          }),

          sdbus::registerMethod("PreviousActive").withOutputParamNames("success").implementedAs([this]() {
            return onPreviousActive();
          })
      )
      .forInterface(kNoctaliaMprisInterface);
}

void MprisService::emitPlayersChanged() {
  std::vector<std::map<std::string, sdbus::Variant>> players;
  for (const auto& player : listPlayers()) {
    players.push_back(to_dbus_player(player));
  }

  m_controlObject->emitSignal("PlayersChanged").onInterface(kNoctaliaMprisInterface).withArguments(players);
}

void MprisService::emitActivePlayerChanged() {
  const auto active = activePlayer();
  if (!active.has_value()) {
    m_controlObject->emitSignal("ActivePlayerChanged")
        .onInterface(kNoctaliaMprisInterface)
        .withArguments(false, std::map<std::string, sdbus::Variant>{});
    return;
  }

  m_controlObject->emitSignal("ActivePlayerChanged")
      .onInterface(kNoctaliaMprisInterface)
      .withArguments(true, to_dbus_player(*active));
}

void MprisService::emitTrackChanged(const MprisPlayerInfo& player) {
  m_controlObject->emitSignal("TrackChanged")
      .onInterface(kNoctaliaMprisInterface)
      .withArguments(player.busName, to_dbus_player(player));
}

void MprisService::syncSignals(const std::optional<MprisPlayerInfo>& previousActive) {
  const auto current_active = activePlayer();
  const std::string current_active_name = current_active.has_value() ? current_active->busName : std::string{};

  if (current_active_name != m_lastEmittedActivePlayer) {
    emitActivePlayerChanged();
    m_lastEmittedActivePlayer = current_active_name;
  }

  if (previousActive.has_value()
      && current_active.has_value()
      && previousActive->busName == current_active->busName
      && previousActive->title != current_active->title) {
    emitTrackChanged(*current_active);
  }
}

void MprisService::registerBusSignals() {
  m_dbusProxy->uponSignal("NameOwnerChanged")
      .onInterface(kDbusInterface)
      .call([this](const std::string& name, const std::string& old_owner, const std::string& new_owner) {
        if (!is_mpris_bus_name(name)) {
          return;
        }

        kLog.debug("name owner changed name={} old_owner=\"{}\" new_owner=\"{}\"", name, old_owner, new_owner);

        if (new_owner.empty()) {
          removePlayer(name);
          return;
        }

        if (old_owner.empty()) {
          addOrRefreshPlayer(name);
          return;
        }

        addOrRefreshPlayer(name);
      });
}

void MprisService::discoverPlayers() {
  try {
    const std::weak_ptr<void> aliveGuard = m_aliveGuard;
    m_dbusProxy->callMethodAsync("ListNames")
        .onInterface(kDbusInterface)
        .uponReplyInvoke([this, aliveGuard](std::optional<sdbus::Error> err, std::vector<std::string> names) {
          if (aliveGuard.expired()) {
            return;
          }
          if (err.has_value()) {
            kLog.warn("discover players failed err={}", err->what());
            scheduleRecoveryDiscovery();
            return;
          }

          m_pendingDiscoveryBusNames.clear();
          for (const auto& name : names) {
            if (is_mpris_bus_name(name)) {
              m_pendingDiscoveryBusNames.push_back(name);
            }
          }
          scheduleDiscoveryDrain();
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("discover players failed err={}", e.what());
    scheduleRecoveryDiscovery();
    return;
  }
}

void MprisService::scheduleDiscoveryDrain() {
  if (m_discoveryDrainScheduled || m_pendingDiscoveryBusNames.empty()) {
    return;
  }

  m_discoveryDrainScheduled = true;
  const std::weak_ptr<void> aliveGuard = m_aliveGuard;
  DeferredCall::callLater([this, aliveGuard]() {
    m_discoveryDrainScheduled = false;
    if (aliveGuard.expired() || m_pendingDiscoveryBusNames.empty()) {
      return;
    }

    const std::string busName = std::move(m_pendingDiscoveryBusNames.front());
    m_pendingDiscoveryBusNames.pop_front();
    addOrRefreshPlayer(busName);

    if (!m_pendingDiscoveryBusNames.empty()) {
      scheduleDiscoveryDrain();
    }
  });
}

void MprisService::scheduleStartupRediscovery() {
  if (m_startupRediscoveryPassesRemaining <= 0) {
    return;
  }

  const std::weak_ptr<void> aliveGuard = m_aliveGuard;
  DeferredCall::callLater([this, aliveGuard]() {
    if (aliveGuard.expired()) {
      return;
    }
    discoverPlayers();
    --m_startupRediscoveryPassesRemaining;
    if (m_startupRediscoveryPassesRemaining > 0) {
      scheduleStartupRediscovery();
    }
  });
}

void MprisService::scheduleRecoveryDiscovery() {
  if (m_recoveryTimer.active()) {
    return;
  }

  static constexpr std::chrono::milliseconds kMaxBackoff{30'000};
  const auto delay = m_recoveryBackoffMs;
  m_recoveryBackoffMs = std::min(m_recoveryBackoffMs * 2, kMaxBackoff);

  const std::weak_ptr<void> aliveGuard = m_aliveGuard;
  m_recoveryTimer.start(delay, [this, aliveGuard]() {
    if (aliveGuard.expired()) {
      return;
    }
    discoverPlayers();
  });
}

void MprisService::addOrRefreshPlayer(const std::string& busName) {
  auto [proxyIt, inserted] =
      m_playerProxies.emplace(busName, sdbus::createProxy(m_bus.connection(), sdbus::ServiceName{busName}, kMprisPath));

  if (inserted) {
    proxyIt->second->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this, busName](
                  const std::string& interface_name, const std::map<std::string, sdbus::Variant>& changed_properties,
                  const std::vector<std::string>& invalidated_properties
              ) {
          if (interface_name == kMprisRootInterface || interface_name == kMprisPlayerInterface) {
            const bool metadataChanged = changed_properties.contains("Metadata")
                || std::ranges::find(invalidated_properties, std::string{"Metadata"}) != invalidated_properties.end();
            const bool positionChanged = changed_properties.contains("Position")
                || std::ranges::find(invalidated_properties, std::string{"Position"}) != invalidated_properties.end();
            if (positionChanged) {
              m_pendingPositionSignalRefresh[busName] = true;
            }
            const auto now = std::chrono::steady_clock::now();
            if (metadataChanged) {
              // Metadata updates often arrive in short bursts (first partial, then full artwork/title payload).
              // Never debounce these or we can get stuck on stale app/logo art after rapid stream switches.
              m_lastPropertiesUpdate[busName] = now;
              addOrRefreshPlayer(busName);
              return;
            }

            const auto last_it = m_lastPropertiesUpdate.find(busName);
            if (last_it != m_lastPropertiesUpdate.end() && now - last_it->second < kPropertiesDebounceWindow) {
              return;
            }
            m_lastPropertiesUpdate[busName] = now;
            addOrRefreshPlayer(busName);
          }
        });

    proxyIt->second->uponSignal("Seeked").onInterface(kMprisPlayerInterface).call([this, busName](int64_t posUs) {
      auto playerIt = m_players.find(busName);
      if (playerIt == m_players.end()) {
        return;
      }

      const auto now = std::chrono::steady_clock::now();
      auto offsetIt = m_positionOffsetsUs.find(busName);
      std::int64_t offsetUs = offsetIt != m_positionOffsetsUs.end() ? offsetIt->second : 0;
      std::int64_t normalizedUs = std::max<std::int64_t>(0, posUs - offsetUs);
      const std::int64_t previousPosUs = playerIt->second.positionUs;
      const auto previousTrackRawIt = m_previousTrackRawPositionUs.find(busName);
      const bool looksLikePreviousTrackContinuation = previousTrackRawIt != m_previousTrackRawPositionUs.end()
          && std::llabs(posUs - previousTrackRawIt->second) <= kPreviousTrackContinuationSlackUs;

      if (looksLikePreviousTrackContinuation) {
        return;
      }

      if (offsetUs > 0) {
        offsetIt->second = 0;
        offsetUs = 0;
        normalizedUs = std::max<std::int64_t>(0, posUs);
      }

      if (!isPlausibleTrackPosition(normalizedUs, playerIt->second.lengthUs)) {
        return;
      }

      if (playerIt->second.playbackStatus == "Paused" && normalizedUs > 0) {
        const auto pauseIt = m_recentNoSignalPauseAt.find(busName);
        const bool recoveringRecentPause =
            pauseIt != m_recentNoSignalPauseAt.end() && now - pauseIt->second <= kNoSignalPauseRecoveryWindow;
        const auto seekCommandIt = m_lastSeekCommandAt.find(busName);
        const bool recentLocalSeek =
            seekCommandIt != m_lastSeekCommandAt.end() && now - seekCommandIt->second <= kSeekPauseGraceWindow;
        const std::int64_t pausedJumpUs = std::llabs(normalizedUs - previousPosUs);
        if (recoveringRecentPause && !recentLocalSeek && pausedJumpUs >= kPauseRecoveryMinJumpUs) {
          playerIt->second.playbackStatus = "Playing";
          m_recentNoSignalPauseAt.erase(pauseIt);
        }
      }

      playerIt->second.positionUs = normalizedUs;
      m_hasAuthoritativePositionSample[busName] = true;
      m_lastPositionSampleAt[busName] = now;
      m_previousTrackRawPositionUs.erase(busName);
      m_pendingPositionCandidateUs.erase(busName);
      m_pendingPositionCandidateMatches.erase(busName);
      m_pendingPositionCandidateAt.erase(busName);
      if (m_changeCallback) {
        m_changeCallback();
      }
    });
  }

  const bool hadPositionSignal = m_pendingPositionSignalRefresh[busName];
  m_pendingPositionSignalRefresh[busName] = false;

  const std::weak_ptr<void> aliveGuard = m_aliveGuard;
  try {
    proxyIt->second->callMethodAsync("GetAll")
        .onInterface(kPropertiesInterface)
        .withArguments(std::string{kMprisRootInterface})
        .uponReplyInvoke([this, aliveGuard, busName, hadPositionSignal](
                             std::optional<sdbus::Error> rootErr, std::map<std::string, sdbus::Variant> rootProps
                         ) {
          if (aliveGuard.expired()) {
            return;
          }

          auto proxyLookup = m_playerProxies.find(busName);
          if (proxyLookup == m_playerProxies.end()) {
            return;
          }

          try {
            proxyLookup->second->callMethodAsync("GetAll")
                .onInterface(kPropertiesInterface)
                .withArguments(std::string{kMprisPlayerInterface})
                .uponReplyInvoke([this, aliveGuard, busName, hadPositionSignal, rootErr,
                                  rootProps = std::move(rootProps)](
                                     std::optional<sdbus::Error> playerErr,
                                     std::map<std::string, sdbus::Variant> playerProps
                                 ) {
                  if (aliveGuard.expired()) {
                    return;
                  }

                  const bool rootFailed = rootErr.has_value();
                  const bool playerFailed = playerErr.has_value();
                  const bool hadFullRefreshFailure = rootFailed && playerFailed;

                  // Track full-properties fetch failures and give up after threshold.
                  if (hadFullRefreshFailure) {
                    int& failureCount = m_playerPropertiesFailures[busName];
                    ++failureCount;

                    if (failureCount == kPlayerPropertiesFailureThreshold) {
                      kLog.warn("player properties refresh disabled after {} failures name={}", failureCount, busName);
                      // Cancel the timer to stop polling this player
                      if (auto it = m_positionResyncTimers.find(busName); it != m_positionResyncTimers.end()) {
                        TimerManager::instance().cancel(it->second);
                        m_positionResyncTimers.erase(it);
                      }
                      return;
                    } else if (failureCount > kPlayerPropertiesFailureThreshold) {
                      return;
                    } else if (failureCount > 1) {
                      // Keep the first failure on the normal retry cadence before backing off.
                      // Apply exponential backoff
                      auto& backoff = m_playerPropertiesRefreshBackoffMs[busName];
                      if (backoff.count() == 0) {
                        backoff = kPositionRetryInitialBackoff;
                      } else {
                        backoff = std::min(backoff * 2, kPositionRetryMaxBackoff);
                      }
                      kLog.debug(
                          "player properties refresh backoff failures={} interval={}ms name={}", failureCount,
                          backoff.count(), busName
                      );
                    }
                  }

                  // For a player we've never seen before, avoid creating a root-only placeholder
                  // entry when the player interface is unavailable. Let recovery rediscover it once
                  // we can read actual playback/metadata state.
                  if (playerFailed && !m_players.contains(busName)) {
                    if (rootFailed) {
                      kLog.warn("player hydration failed (both interfaces) name={}", busName);
                    } else {
                      kLog.warn("player hydration failed (player interface) name={}", busName);
                    }
                    scheduleRecoveryDiscovery();
                    return;
                  }

                  std::map<std::string, sdbus::Variant> effectiveRootProps;
                  if (!rootFailed) {
                    effectiveRootProps = rootProps;
                  }

                  std::map<std::string, sdbus::Variant> effectivePlayerProps;
                  if (!playerFailed) {
                    effectivePlayerProps = playerProps;
                  }

                  const MprisPlayerInfo info =
                      readPlayerInfoFromProperties(busName, effectiveRootProps, effectivePlayerProps);
                  if (!playerFailed && !hasStrongNowPlayingMetadata(info)) {
                    removePlayerCacheEntry(busName);
                    return;
                  }
                  applyPlayerSnapshot(busName, info, hadPositionSignal, hadFullRefreshFailure);
                });
          } catch (const sdbus::Error& e) {
            kLog.warn("player query failed name={} err={}", busName, e.what());
            scheduleRecoveryDiscovery();
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("player query failed name={} err={}", busName, e.what());
    scheduleRecoveryDiscovery();
  }
}

void MprisService::applyPlayerSnapshot(
    const std::string& busName, const MprisPlayerInfo& info, bool hadPositionSignal, bool hadFullRefreshFailure
) {
  m_recoveryBackoffMs = std::chrono::milliseconds{500};
  if (!hadFullRefreshFailure) {
    m_playerPropertiesFailures[busName] = 0;
    m_playerPropertiesRefreshBackoffMs.erase(busName);
  }
  const auto previousActive = activePlayer();
  const auto now = std::chrono::steady_clock::now();

  if (info.playbackStatus == "Playing") {
    m_stoppedPlayers.erase(busName);
    m_lastActivePlayer = busName;
    m_lastPlayingUpdate[busName] = now;
  }
  if (hasStrongNowPlayingMetadata(info)) {
    m_lastStrongMetadataUpdate[busName] = now;
  }

  const auto existing = m_players.find(busName);
  if (existing == m_players.end()) {
    MprisPlayerInfo initial = info;
    if (!hadPositionSignal) {
      initial.positionUs = 0;
    }
    m_logicalTrackSignatures[busName] = logicalTrackSignature(initial);
    m_positionOffsetsUs[busName] = 0;
    m_lastLogicalTrackChangeAt[busName] = now;
    m_lastPositionSampleAt[busName] = now;
    m_hasAuthoritativePositionSample[busName] = false;
    m_previousTrackRawPositionUs.erase(busName);
    m_pendingPositionCandidateUs.erase(busName);
    m_pendingPositionCandidateMatches.erase(busName);
    m_pendingPositionCandidateAt.erase(busName);
    m_players.emplace(busName, std::move(initial));
    emitPlayersChanged();
    syncSignals(previousActive);
    if (m_changeCallback) {
      m_changeCallback();
    }

    if (info.playbackStatus != "Stopped" || info.positionUs > 0) {
      const std::weak_ptr<void> aliveGuard = m_aliveGuard;
      DeferredCall::callLater([this, aliveGuard, busName]() {
        if (aliveGuard.expired()) {
          return;
        }
        refreshPlayerPosition(busName, true);
      });
      if (shouldRetryPropertiesRefresh(busName)) {
        schedulePositionRefreshRetry(busName, kPositionRetryInterval, true);
      }
    }
    return;
  }

  if (existing->second != info) {
    const MprisPlayerInfo previous_info = existing->second;

    MprisPlayerInfo merged = info;
    if (merged.artUrl.empty() && !previous_info.artUrl.empty()) {
      merged.artUrl = previous_info.artUrl;
    }

    const bool incomingSnapshotEmpty = merged.playbackStatus.empty()
        && merged.trackId.empty()
        && merged.title.empty()
        && merged.artists.empty()
        && merged.album.empty()
        && merged.sourceUrl.empty()
        && merged.artUrl.empty()
        && merged.lengthUs == 0;
    if (incomingSnapshotEmpty) {
      merged = previous_info;
    }

    const bool previousStrong = hasStrongNowPlayingMetadata(previous_info) || !previous_info.artUrl.empty();
    const bool incomingWeak = !hasStrongNowPlayingMetadata(info);
    const auto strongIt = m_lastStrongMetadataUpdate.find(busName);
    const bool withinStabilizeWindow =
        strongIt != m_lastStrongMetadataUpdate.end() && now - strongIt->second < kMetadataStabilizeWindow;
    const auto seekCommandIt = m_lastSeekCommandAt.find(busName);
    const bool recentLocalSeek =
        seekCommandIt != m_lastSeekCommandAt.end() && now - seekCommandIt->second <= kSeekPauseGraceWindow;

    if (merged.playbackStatus == "Playing"
        && previousStrong
        && incomingWeak
        && withinStabilizeWindow
        && recentLocalSeek) {
      const std::string incomingArtUrl = info.artUrl;
      const std::string incomingSourceUrl = info.sourceUrl;
      merged.trackId = previous_info.trackId;
      merged.title = previous_info.title;
      merged.artists = previous_info.artists;
      merged.album = previous_info.album;
      merged.sourceUrl = previous_info.sourceUrl;
      merged.artUrl = previous_info.artUrl;
      if (!incomingArtUrl.empty()) {
        merged.artUrl = incomingArtUrl;
      }
      if (!incomingSourceUrl.empty()) {
        merged.sourceUrl = incomingSourceUrl;
      }
    }

    const std::string newSignature = logicalTrackSignature(merged);
    const auto signatureIt = m_logicalTrackSignatures.find(busName);
    const std::string previousSignature =
        signatureIt != m_logicalTrackSignatures.end() ? signatureIt->second : logicalTrackSignature(previous_info);

    auto offsetIt = m_positionOffsetsUs.find(busName);
    if (offsetIt == m_positionOffsetsUs.end()) {
      offsetIt = m_positionOffsetsUs.emplace(busName, 0).first;
    }

    const bool previousPositionAuthoritative =
        m_hasAuthoritativePositionSample.contains(busName) && m_hasAuthoritativePositionSample.at(busName);

    const bool logicalTrackChanged = !newSignature.empty() && previousSignature != newSignature;
    const bool playbackStatusChanged = previous_info.playbackStatus != merged.playbackStatus;

    if (!logicalTrackChanged
        && merged.lengthUs == 0
        && previous_info.lengthUs > 0
        && merged.playbackStatus != "Stopped") {
      merged.lengthUs = previous_info.lengthUs;
    }

    if (previousPositionAuthoritative
        && !logicalTrackChanged
        && !hadPositionSignal
        && previous_info.playbackStatus == "Playing"
        && merged.playbackStatus == "Paused"
        && previous_info.canSeek) {
      m_recentNoSignalPauseAt[busName] = now;
    } else if (merged.playbackStatus != "Paused") {
      m_recentNoSignalPauseAt.erase(busName);
    }

    bool preservedNormalizedPosition = false;
    if (previousPositionAuthoritative
        && !logicalTrackChanged
        && !hadPositionSignal
        && previous_info.playbackStatus != "Stopped"
        && merged.playbackStatus != "Stopped"
        && previous_info.positionUs != merged.positionUs) {
      bool preservePreviousPosition = playbackStatusChanged;
      if (!preservePreviousPosition) {
        const auto sampleIt = m_lastPositionSampleAt.find(busName);
        const std::int64_t elapsedSinceSampleUs = sampleIt != m_lastPositionSampleAt.end()
            ? std::chrono::duration_cast<std::chrono::microseconds>(now - sampleIt->second).count()
            : 0;
        const std::int64_t rawDeltaUs = std::llabs(merged.positionUs - previous_info.positionUs);
        const std::int64_t maxReasonableDeltaUs = std::max<std::int64_t>(5'000'000, elapsedSinceSampleUs + 2'000'000);
        preservePreviousPosition = rawDeltaUs > maxReasonableDeltaUs;
        if (!preservePreviousPosition
            && previous_info.playbackStatus == "Paused"
            && merged.playbackStatus == "Paused") {
          preservePreviousPosition = !recentLocalSeek && rawDeltaUs < kPausedSameTrackPositionJumpToleranceUs;
        }
      }

      if (preservePreviousPosition) {
        merged.positionUs = previous_info.positionUs;
        preservedNormalizedPosition = true;
      }
    }
    if (previousPositionAuthoritative
        && !logicalTrackChanged
        && !hadPositionSignal
        && merged.positionUs == 0
        && previous_info.positionUs > 0
        && previous_info.playbackStatus != "Stopped"
        && merged.playbackStatus != "Stopped") {
      merged.positionUs = previous_info.positionUs;
      preservedNormalizedPosition = true;
    }
    if (logicalTrackChanged) {
      const std::int64_t previousOffset = offsetIt->second;
      const std::int64_t previousNormalized = std::max<std::int64_t>(0, previous_info.positionUs - previousOffset);
      const std::int64_t previousRawPositionUs = std::max<std::int64_t>(0, previous_info.positionUs + previousOffset);
      m_previousTrackRawPositionUs[busName] = previousRawPositionUs;
      offsetIt->second = 0;
      const bool looksLikePreviousTrackContinuation = merged.positionUs > 5'000'000
          && previousNormalized > 5'000'000
          && std::llabs(merged.positionUs - previousRawPositionUs) <= kPreviousTrackContinuationSlackUs;
      if (looksLikePreviousTrackContinuation) {
        offsetIt->second = merged.positionUs;
      }

      m_lastLogicalTrackChangeAt[busName] = now;
      m_hasAuthoritativePositionSample[busName] = false;
      m_recentNoSignalPauseAt.erase(busName);
      m_lastPositionSampleAt[busName] = now;
      m_pendingPositionCandidateUs.erase(busName);
      m_pendingPositionCandidateMatches.erase(busName);
      m_pendingPositionCandidateAt.erase(busName);
    }

    if (!preservedNormalizedPosition) {
      const std::int64_t offsetUs = offsetIt->second;
      merged.positionUs = std::max<std::int64_t>(0, merged.positionUs - offsetUs);
    }
    m_logicalTrackSignatures[busName] = newSignature;
    if (hadPositionSignal) {
      m_hasAuthoritativePositionSample[busName] = true;
    }
    if (previous_info.positionUs != merged.positionUs || previous_info.playbackStatus != merged.playbackStatus) {
      m_lastPositionSampleAt[busName] = now;
    }

    existing->second = merged;

    const bool trackChanged = previous_info.title != merged.title
        || previous_info.album != merged.album
        || previous_info.artists != merged.artists
        || previous_info.artUrl != merged.artUrl
        || previous_info.sourceUrl != merged.sourceUrl
        || previous_info.trackId != merged.trackId
        || previous_info.lengthUs != merged.lengthUs;
    const bool significantChanged = trackChanged
        || previous_info.identity != merged.identity
        || previous_info.playbackStatus != merged.playbackStatus
        || previous_info.loopStatus != merged.loopStatus
        || previous_info.shuffle != merged.shuffle
        || previous_info.canGoPrevious != merged.canGoPrevious
        || previous_info.canGoNext != merged.canGoNext
        || previous_info.canPlay != merged.canPlay
        || previous_info.canPause != merged.canPause
        || previous_info.canSeek != merged.canSeek;

    if (trackChanged || previous_info.playbackStatus != merged.playbackStatus) {
      const std::weak_ptr<void> aliveGuard = m_aliveGuard;
      DeferredCall::callLater([this, aliveGuard, busName]() {
        if (aliveGuard.expired()) {
          return;
        }
        refreshPlayerPosition(busName, true);
      });
      if (shouldRetryPropertiesRefresh(busName)) {
        schedulePositionRefreshRetry(busName, kPositionRetryInterval, true);
      }
    }

    if (trackChanged) {
      emitTrackChanged(merged);
    }

    syncSignals(previousActive);
    if (significantChanged && m_changeCallback) {
      m_changeCallback();
    }
  }
}

void MprisService::removePlayerCacheEntry(const std::string& busName) {
  const auto previousActive = activePlayer();
  const bool hadPlayer = m_players.contains(busName);

  clearPlayerState(busName);

  if (!hadPlayer) {
    return;
  }

  emitPlayersChanged();
  syncSignals(previousActive);
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void MprisService::clearPlayerState(const std::string& busName) {
  m_players.erase(busName);
  m_logicalTrackSignatures.erase(busName);
  m_positionOffsetsUs.erase(busName);
  if (auto it = m_positionResyncTimers.find(busName); it != m_positionResyncTimers.end()) {
    TimerManager::instance().cancel(it->second);
    m_positionResyncTimers.erase(it);
  }
  m_pendingPositionSignalRefresh.erase(busName);
  m_hasAuthoritativePositionSample.erase(busName);
  m_lastLogicalTrackChangeAt.erase(busName);
  m_previousTrackRawPositionUs.erase(busName);
  m_lastSeekCommandAt.erase(busName);
  m_recentNoSignalPauseAt.erase(busName);
  m_pendingPositionCandidateUs.erase(busName);
  m_pendingPositionCandidateMatches.erase(busName);
  m_pendingPositionCandidateAt.erase(busName);
  m_lastPositionSampleAt.erase(busName);
  m_lastPropertiesUpdate.erase(busName);
  m_lastPlayingUpdate.erase(busName);
  m_lastStrongMetadataUpdate.erase(busName);
  m_playerPropertiesFailures.erase(busName);
  m_playerPropertiesRefreshBackoffMs.erase(busName);
  if (m_lastActivePlayer == busName) {
    m_lastActivePlayer.clear();
  }
  m_stoppedPlayers.erase(busName);
}

void MprisService::removePlayer(const std::string& busName) {
  const auto previousActive = activePlayer();

  if (!m_players.contains(busName) && !m_playerProxies.contains(busName)) {
    return;
  }

  clearPlayerState(busName);
  m_playerProxies.erase(busName);
  emitPlayersChanged();
  syncSignals(previousActive);
  if (m_changeCallback) {
    m_changeCallback();
  }

  // Name-owner churn can race with our own cache updates. Re-run discovery
  // on the next loop tick so transient gaps do not leave media UI empty.
  scheduleRecoveryDiscovery();
}

std::optional<std::string> MprisService::chooseActivePlayer() const {
  const auto isDismissed = [this](const std::string& busName) { return m_stoppedPlayers.contains(busName); };

  if (m_pinnedPlayerPreference.has_value()) {
    const auto it = m_players.find(*m_pinnedPlayerPreference);
    if (it != m_players.end() && !isBlacklisted(it->second) && !isDismissed(*m_pinnedPlayerPreference)) {
      // kLog.debug("choose active player source=pinned name={}", *m_pinnedPlayerPreference);
      return *m_pinnedPlayerPreference;
    }
  }

  std::optional<std::string> mostRecentPlaying;
  std::chrono::steady_clock::time_point mostRecentPlayingAt{};
  for (const auto& [busName, player] : m_players) {
    if (isBlacklisted(player) || isDismissed(busName) || player.playbackStatus != "Playing") {
      continue;
    }
    const auto playingIt = m_lastPlayingUpdate.find(busName);
    const auto seenAt =
        playingIt != m_lastPlayingUpdate.end() ? playingIt->second : std::chrono::steady_clock::time_point{};
    if (!mostRecentPlaying.has_value() || seenAt > mostRecentPlayingAt) {
      mostRecentPlaying = busName;
      mostRecentPlayingAt = seenAt;
    }
  }
  if (mostRecentPlaying.has_value()) {
    // kLog.debug("choose active player source=recent_playing name={}", *mostRecentPlaying);
    return mostRecentPlaying;
  }

  for (const auto& busName : m_preferredPlayers) {
    const auto it = m_players.find(busName);
    if (it != m_players.end()
        && !isBlacklisted(it->second)
        && !isDismissed(busName)
        && it->second.playbackStatus == "Playing") {
      // kLog.debug("choose active player source=preferred_playing name={}", busName);
      return busName;
    }
  }

  for (const auto& busName : m_preferredPlayers) {
    const auto it = m_players.find(busName);
    if (it != m_players.end() && !isBlacklisted(it->second) && !isDismissed(busName)) {
      // kLog.debug("choose active player source=preferred_any name={}", busName);
      return busName;
    }
  }

  if (!m_lastActivePlayer.empty()) {
    const auto it = m_players.find(m_lastActivePlayer);
    if (it != m_players.end() && !isBlacklisted(it->second) && !isDismissed(m_lastActivePlayer)) {
      // kLog.debug("choose active player source=last_active name={}", m_lastActivePlayer);
      return m_lastActivePlayer;
    }
  }

  for (const auto& [busName, player] : m_players) {
    if (!isBlacklisted(player) && !isDismissed(busName)) {
      // kLog.debug("choose active player source=first_cached name={}", busName);
      return busName;
    }
  }

  // kLog.debug("choose active player source=none");
  return std::nullopt;
}

bool MprisService::isBlacklisted(const MprisPlayerInfo& player) const {
  if (m_blacklist.empty()) {
    return false;
  }

  const std::string busName = normalizeFilterToken(player.busName);
  const std::string identity = normalizeFilterToken(player.identity);
  const std::string desktopEntry = normalizeFilterToken(player.desktopEntry);

  for (const auto& token : m_blacklist) {
    if (token == busName || token == identity || token == desktopEntry) {
      return true;
    }
    if (!token.empty() && busName.find(token) != std::string::npos) {
      return true;
    }
  }

  return false;
}

bool MprisService::callPlayerMethod(const std::string& busName, const char* methodName) {
  const auto it = m_playerProxies.find(busName);
  if (it == m_playerProxies.end()) {
    return false;
  }

  const std::string method{methodName}; // Capture as owned string, not dangling pointer

  try {
    it->second->callMethodAsync(method.c_str())
        .onInterface(kMprisPlayerInterface)
        .uponReplyInvoke(makeAsyncReplyHandler("control", busName, method));
    return true;
  } catch (const sdbus::Error& e) {
    kLog.warn("control dispatch failed name={} method={} err={}", busName, method, e.what());
    return false;
  }
}

bool MprisService::canInvoke(const MprisPlayerInfo& player, const char* methodName) const {
  const std::string_view method{methodName};
  if (method == "PlayPause" || method == "Stop") {
    return player.canPlay || player.canPause;
  }
  if (method == "Next") {
    return player.canGoNext;
  }
  if (method == "Previous") {
    return player.canGoPrevious;
  }
  return false;
}

void MprisService::dismissPlayer(const std::string& busName) {
  if (busName.empty()) {
    return;
  }

  const auto previousActive = chooseActivePlayer();
  m_stoppedPlayers.insert(busName);
  if (m_lastActivePlayer == busName) {
    m_lastActivePlayer.clear();
  }

  if (!previousActive.has_value() || *previousActive != busName) {
    return;
  }

  emitActivePlayerChanged();
  if (m_changeCallback) {
    m_changeCallback();
  }
}

bool MprisService::onPlayPausePlayer(const std::string& busName) {
  if (busName.empty()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.InvalidArgs"}, "player_bus_name must not be empty");
  }

  if (!m_players.contains(busName)) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "player was not found");
  }

  const bool ok = playPause(busName);
  if (!ok) {
    throw sdbus::Error(
        sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotSupported"}, "player does not support PlayPause"
    );
  }
  return true;
}

bool MprisService::onStopPlayer(const std::string& busName) {
  if (busName.empty()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.InvalidArgs"}, "player_bus_name must not be empty");
  }

  if (!m_players.contains(busName)) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "player was not found");
  }

  const bool ok = stop(busName);
  if (!ok) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotSupported"}, "player does not support Stop");
  }
  return true;
}

bool MprisService::onNextPlayer(const std::string& busName) {
  if (busName.empty()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.InvalidArgs"}, "player_bus_name must not be empty");
  }

  if (!m_players.contains(busName)) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "player was not found");
  }

  const bool ok = next(busName);
  if (!ok) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotSupported"}, "player does not support Next");
  }
  return true;
}

bool MprisService::onPreviousPlayer(const std::string& busName) {
  if (busName.empty()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.InvalidArgs"}, "player_bus_name must not be empty");
  }

  if (!m_players.contains(busName)) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "player was not found");
  }

  const bool ok = previous(busName);
  if (!ok) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotSupported"}, "player does not support Previous");
  }
  return true;
}

bool MprisService::onPlayPauseActive() {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "no active player available");
  }
  return onPlayPausePlayer(*active);
}

bool MprisService::onStopActive() {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "no active player available");
  }
  return onStopPlayer(*active);
}

bool MprisService::onNextActive() {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "no active player available");
  }
  return onNextPlayer(*active);
}

bool MprisService::onPreviousActive() {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "no active player available");
  }
  return onPreviousPlayer(*active);
}

bool MprisService::onSeekPlayer(const std::string& busName, int64_t offsetUs) {
  if (busName.empty()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.InvalidArgs"}, "player_bus_name must not be empty");
  }

  if (!m_players.contains(busName)) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "player was not found");
  }

  if (!seek(busName, offsetUs)) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotSupported"}, "player does not support Seek");
  }
  return true;
}

bool MprisService::onSeekActive(int64_t offsetUs) {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "no active player available");
  }
  return onSeekPlayer(*active, offsetUs);
}

bool MprisService::onSetPositionPlayer(const std::string& busName, int64_t positionUs) {
  if (busName.empty()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.InvalidArgs"}, "player_bus_name must not be empty");
  }

  if (!m_players.contains(busName)) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "player was not found");
  }

  if (!setPosition(busName, positionUs)) {
    throw sdbus::Error(
        sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotSupported"}, "player does not support SetPosition"
    );
  }
  return true;
}

bool MprisService::onSetPositionActive(int64_t positionUs) {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "no active player available");
  }
  return onSetPositionPlayer(*active, positionUs);
}

int64_t MprisService::onGetPositionPlayer(const std::string& busName) const {
  if (busName.empty()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.InvalidArgs"}, "player_bus_name must not be empty");
  }

  const auto pos = position(busName);
  if (!pos.has_value()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "player was not found");
  }
  return *pos;
}

int64_t MprisService::onGetPositionActive() const {
  const auto pos = positionActive();
  if (!pos.has_value()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "no active player available");
  }
  return *pos;
}

double MprisService::onGetVolumePlayer(const std::string& busName) const {
  if (busName.empty()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.InvalidArgs"}, "player_bus_name must not be empty");
  }

  const auto currentVolume = volume(busName);
  if (!currentVolume.has_value()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "player was not found");
  }
  return *currentVolume;
}

double MprisService::onGetVolumeActive() const {
  const auto currentVolume = volumeActive();
  if (!currentVolume.has_value()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "no active player available");
  }
  return *currentVolume;
}

bool MprisService::onSetVolumePlayer(const std::string& busName, double volume) {
  if (busName.empty()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.InvalidArgs"}, "player_bus_name must not be empty");
  }

  if (!std::isfinite(volume) || volume < 0.0) {
    throw sdbus::Error(
        sdbus::Error::Name{"dev.noctalia.Mpris.Error.InvalidArgs"}, "volume must be a finite non-negative number"
    );
  }

  if (!m_players.contains(busName)) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "player was not found");
  }

  if (!setVolume(busName, volume)) {
    throw sdbus::Error(
        sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotSupported"}, "player does not support Volume updates"
    );
  }
  return true;
}

bool MprisService::onSetVolumeActive(double volume) {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "no active player available");
  }
  return onSetVolumePlayer(*active, volume);
}

bool MprisService::onGetShufflePlayer(const std::string& busName) const {
  if (busName.empty()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.InvalidArgs"}, "player_bus_name must not be empty");
  }

  const auto currentShuffle = shuffle(busName);
  if (!currentShuffle.has_value()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "player was not found");
  }
  return *currentShuffle;
}

bool MprisService::onGetShuffleActive() const {
  const auto currentShuffle = shuffleActive();
  if (!currentShuffle.has_value()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "no active player available");
  }
  return *currentShuffle;
}

bool MprisService::onSetShufflePlayer(const std::string& busName, bool shuffle) {
  if (busName.empty()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.InvalidArgs"}, "player_bus_name must not be empty");
  }

  if (!m_players.contains(busName)) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "player was not found");
  }

  if (!setShuffle(busName, shuffle)) {
    throw sdbus::Error(
        sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotSupported"}, "player does not support Shuffle updates"
    );
  }
  return true;
}

bool MprisService::onSetShuffleActive(bool shuffle) {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "no active player available");
  }
  return onSetShufflePlayer(*active, shuffle);
}

std::string MprisService::onGetLoopStatusPlayer(const std::string& busName) const {
  if (busName.empty()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.InvalidArgs"}, "player_bus_name must not be empty");
  }

  const auto currentLoopStatus = loopStatus(busName);
  if (!currentLoopStatus.has_value()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "player was not found");
  }
  return *currentLoopStatus;
}

std::string MprisService::onGetLoopStatusActive() const {
  const auto currentLoopStatus = loopStatusActive();
  if (!currentLoopStatus.has_value()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "no active player available");
  }
  return *currentLoopStatus;
}

bool MprisService::onSetLoopStatusPlayer(const std::string& busName, const std::string& loopStatus) {
  if (busName.empty()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.InvalidArgs"}, "player_bus_name must not be empty");
  }

  if (!is_valid_loop_status(loopStatus)) {
    throw sdbus::Error(
        sdbus::Error::Name{"dev.noctalia.Mpris.Error.InvalidArgs"}, "loop_status must be one of: None, Track, Playlist"
    );
  }

  if (!m_players.contains(busName)) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "player was not found");
  }

  if (!setLoopStatus(busName, loopStatus)) {
    throw sdbus::Error(
        sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotSupported"}, "player does not support LoopStatus updates"
    );
  }
  return true;
}

bool MprisService::onSetLoopStatusActive(const std::string& loopStatus) {
  const auto active = chooseActivePlayer();
  if (!active.has_value()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "no active player available");
  }
  return onSetLoopStatusPlayer(*active, loopStatus);
}

bool MprisService::onSetActivePlayerPreference(const std::string& busName) {
  if (busName.empty()) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.InvalidArgs"}, "player_bus_name must not be empty");
  }

  if (!setPinnedPlayerPreference(busName)) {
    throw sdbus::Error(sdbus::Error::Name{"dev.noctalia.Mpris.Error.NotFound"}, "player was not found");
  }
  return true;
}

bool MprisService::onClearActivePlayerPreference() {
  clearPinnedPlayerPreference();
  return true;
}

bool MprisService::onSetPreferredPlayers(const std::vector<std::string>& preferredBusNames) {
  setPreferredPlayers(preferredBusNames);
  return true;
}

std::tuple<bool, std::string, std::vector<std::string>> MprisService::onGetPlayerPreferences() const {
  if (!m_pinnedPlayerPreference.has_value()) {
    return {false, "", m_preferredPlayers};
  }
  return {true, *m_pinnedPlayerPreference, m_preferredPlayers};
}

MprisPlayerInfo MprisService::readPlayerInfoFromProperties(
    const std::string& busName, const std::map<std::string, sdbus::Variant>& rootProps,
    const std::map<std::string, sdbus::Variant>& playerProps
) const {
  auto metadata = get_variant_map_from_props(playerProps, "Metadata");
  std::string trackId = normalizeTrackId(get_object_path_from_variant(metadata, "mpris:trackid"));
  std::vector<std::string> artists = normalizeArtists(get_string_array_from_variant(metadata, "xesam:artist"));

  return MprisPlayerInfo{
      .busName = busName,
      .identity = get_string_from_props(rootProps, "Identity"),
      .desktopEntry = get_string_from_props(rootProps, "DesktopEntry"),
      .playbackStatus = get_string_from_props(playerProps, "PlaybackStatus"),
      .trackId = std::move(trackId),
      .title = get_string_from_variant(metadata, "xesam:title"),
      .artists = std::move(artists),
      .album = get_string_from_variant(metadata, "xesam:album"),
      .sourceUrl = get_string_from_variant(metadata, "xesam:url"),
      .artUrl = get_string_from_variant(metadata, "mpris:artUrl"),
      .loopStatus = get_string_from_props_or(playerProps, "LoopStatus", "None"),
      .shuffle = get_bool_from_props(playerProps, "Shuffle"),
      .volume = get_double_from_props(playerProps, "Volume", 1.0),
      .positionUs = 0,
      .lengthUs = sanitizeLengthUs(get_int64_from_variant(metadata, "mpris:length")),
      .canPlay = get_bool_from_props(playerProps, "CanPlay"),
      .canPause = get_bool_from_props(playerProps, "CanPause"),
      .canGoNext = get_bool_from_props(playerProps, "CanGoNext"),
      .canGoPrevious = get_bool_from_props(playerProps, "CanGoPrevious"),
      .canSeek = get_bool_from_props(playerProps, "CanSeek"),
  };
}
