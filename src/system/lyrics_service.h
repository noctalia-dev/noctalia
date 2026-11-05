#pragma once

#include "core/timer_manager.h"
#include "system/lyrics.h"

#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class ConfigService;
class HttpClient;
struct HttpResponse;
struct MprisPlayerInfo;

// One shared cache and request queue for all lyric consumers.
class LyricsService {
public:
  using Callback = std::function<void(std::shared_ptr<const LyricsResult>)>;

  LyricsService(HttpClient& httpClient, const ConfigService& config);
  void request(const MprisPlayerInfo& player, Callback callback);
  bool offline() const;
  static std::string trackKey(const MprisPlayerInfo& player);
  // Whether `player` is still on the track whose lyrics were found. Requires the same track id, plus one of: a title
  // that wraps the old one; a paused player with the same length; or the same length with playback continuing from
  // `expectedPositionUs`. Chromium reports hover-preview videos this way while the real video keeps playing.
  static bool continuesTrack(
      const MprisPlayerInfo& player, std::string_view trackId, std::string_view title, std::int64_t lengthUs,
      std::optional<std::int64_t> expectedPositionUs = std::nullopt
  );

private:
  enum class Provider : std::uint8_t {
    MUSICBRAINZ,
    DEEZER_SEARCH,
    DEEZER_TRACK,
    LRCMUX,
    LYRICSPLUS,
    LRCLIB,
    KUGOU_SONG,
    KUGOU_SEARCH,
    KUGOU_LYRIC,
    NETEASE_SEARCH,
    NETEASE_LYRIC,
  };

  struct ReadyTrack {
    std::string key;
    std::string trackId;
    std::string title;
  };

  struct Entry {
    std::string busName;
    std::string trackId;
    std::string url;
    LyricsTrack rawTrack;
    LyricsTrack track;
    std::optional<LyricsTrack> pendingTrack;
    std::string isrc;
    std::uint64_t deezerId = 0;
    Provider provider = Provider::MUSICBRAINZ;
    bool hadError = false;
    std::uint64_t priority = 0;
    // Best lyrics found so far and the provider they came from, while later providers are asked for better.
    std::shared_ptr<const LyricsResult> best;
    Provider bestProvider = Provider::LRCMUX;
    // Length of the song matched at the current lyrics-only source, for results that do not report one.
    std::int64_t sourceDurationMs = 0;
    std::shared_ptr<const LyricsResult> result;
    std::vector<Callback> callbacks;
    std::chrono::steady_clock::time_point expiresAt;
  };

  // Players such as Chromium keep reporting the previous track's length after in-page navigation.
  struct LengthSample {
    std::string trackKey;
    std::int64_t lengthUs = 0;
    bool stale = false;
  };

  bool staleLengthFor(const MprisPlayerInfo& player, const std::string& key);
  static std::size_t throttleSlot(Provider provider);
  // Preference between lyrics that are otherwise equal; lower wins.
  static int sourceOrder(Provider provider);
  void enqueue(const std::string& key, Provider provider, std::string url);
  void requestCatalog(const std::string& key);
  void requestMusicBrainz(const std::string& key);
  void requestLyrics(const std::string& key);
  void requestLyricsPlus(const std::string& key);
  void requestLrclib(const std::string& key);
  void requestKugou(const std::string& key);
  void requestNetease(const std::string& key);
  std::string sourceQuery(const Entry& entry) const;
  void consider(Entry& entry, std::shared_ptr<const LyricsResult> result);
  bool settled(const Entry& entry) const;
  void finishBest(const std::string& key);
  void startNext();
  void finish(const std::string& key, HttpResponse response);
  void complete(const std::string& key, const std::shared_ptr<const LyricsResult>& result);
  void trimCache();

  HttpClient& m_httpClient;
  const ConfigService& m_config;
  std::unordered_map<std::string, Entry> m_cache;
  std::unordered_map<std::string, LengthSample> m_lastLengths;
  std::unordered_map<std::string, ReadyTrack> m_lastReady;
  std::deque<std::string> m_queue;
  std::uint64_t m_priorityCounter = 0;
  bool m_busy = false;
  std::array<std::chrono::steady_clock::time_point, 7> m_nextRequestAt;
  // Sources that could not be reached are skipped until then.
  std::array<std::chrono::steady_clock::time_point, 7> m_backoffUntil;
  Timer m_requestTimer;
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);
};
