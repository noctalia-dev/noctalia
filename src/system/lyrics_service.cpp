#include "system/lyrics_service.h"

#include "app/build_info.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/mpris/mpris_service.h"
#include "net/http_client.h"
#include "net/uri.h"
#include "system/lyrics_catalog.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

namespace {
  constexpr Logger kLog("lyrics");
  constexpr std::array<std::string_view, 11> kProviderNames = {
      "musicbrainz", "deezer-search", "deezer-track", "lrcmux",         "lyricsplus",    "lrclib",
      "kugou-song",  "kugou-search",  "kugou-lyric",  "netease-search", "netease-lyric",
  };
  constexpr std::size_t kMaxCacheEntries = 64;
  constexpr auto kRequestSpacing = std::chrono::milliseconds{300};
  constexpr auto kCatalogSpacing = std::chrono::milliseconds{1100};
  constexpr auto kErrorLifetime = std::chrono::seconds{5};
  // A provider that cannot be reached is skipped for this long.
  constexpr auto kProviderBackoff = std::chrono::minutes{10};
  // Lyrics timed for a recording this close to the player's length were made for what is playing.
  constexpr std::int64_t kMinLengthSlackMs = 2000;

  bool lengthMatches(const LyricsResult& result, std::int64_t wantedMs) {
    if (wantedMs <= 0 || result.recordingDurationMs <= 0) {
      return false;
    }
    return std::llabs(result.recordingDurationMs - wantedMs) <= std::max(kMinLengthSlackMs, wantedMs / 100);
  }

  // Playback that lands this close to where it was expected to be has not changed track.
  constexpr std::int64_t kPositionContinuityUs = 3000000;

  LyricsTrack lookupTrack(const MprisPlayerInfo& player, bool staleLength) {
    return {
        .title = player.title,
        .artist = joinedArtists(player.artists),
        .album = player.album,
        .durationUs = staleLength ? 0 : player.lengthUs,
    };
  }

  // Word timing beats line timing beats plain text.
  int lyricsQuality(const LyricsResult& result) {
    if (result.status != LyricsStatus::READY) {
      return 0;
    }
    if (std::ranges::any_of(result.document.lines, [](const LyricsLine& line) { return !line.words.empty(); })) {
      return 3;
    }
    if (!result.document.lines.empty()) {
      return 2;
    }
    return result.document.plainLines.empty() ? 0 : 1;
  }

  bool meaningfulMetadataChanged(const LyricsTrack& previous, const LyricsTrack& current) {
    if (!current.album.empty() && current.album != previous.album) {
      return true;
    }
    if (current.durationUs <= 0) {
      return false;
    }
    if (previous.durationUs <= 0) {
      return true;
    }
    const auto difference = current.durationUs >= previous.durationUs ? current.durationUs - previous.durationUs
                                                                      : previous.durationUs - current.durationUs;
    return difference >= 1000000;
  }

  constexpr std::uintmax_t kMaxSidecarBytes = 512 * 1024;

  // Lyrics stored beside a local audio file as `<name>.lrc`.
  std::shared_ptr<const LyricsResult> sidecarLyrics(const MprisPlayerInfo& player) {
    if (!player.sourceUrl.starts_with("file://")) {
      return nullptr;
    }
    std::filesystem::path path = uri::normalizeFileUrl(player.sourceUrl);
    if (path.empty()) {
      return nullptr;
    }
    path.replace_extension(".lrc");
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > kMaxSidecarBytes) {
      return nullptr;
    }
    std::ifstream file(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!file.read(text.data(), static_cast<std::streamsize>(size))) {
      return nullptr;
    }
    auto result = std::make_shared<LyricsResult>();
    result->document.lines = parseLrc(text);
    if (result->document.lines.empty()) {
      result->document.plainLines = splitLyricsLines(text);
    }
    if (result->document.lines.empty() && result->document.plainLines.empty()) {
      return nullptr;
    }
    result->status = LyricsStatus::READY;
    kLog.debug("using sidecar lyrics {}", path.string());
    return result;
  }

  std::optional<std::string> deezerIsrc(std::string_view body, std::uint64_t expectedId) {
    try {
      const auto record = nlohmann::json::parse(body);
      if (record.at("id").get<std::uint64_t>() != expectedId) {
        return std::nullopt;
      }
      auto isrc = record.at("isrc").get<std::string>();
      if (isrc.size() != 12) {
        return std::nullopt;
      }
      for (std::size_t i = 0; i < isrc.size(); ++i) {
        auto& ch = isrc[i];
        if (ch >= 'a' && ch <= 'z') {
          ch = static_cast<char>(ch - 'a' + 'A');
        }
        const bool letter = ch >= 'A' && ch <= 'Z';
        const bool digit = ch >= '0' && ch <= '9';
        if ((i < 2 && !letter) || (i >= 2 && i < 5 && !letter && !digit) || (i >= 5 && !digit)) {
          return std::nullopt;
        }
      }
      return isrc;
    } catch (const nlohmann::json::exception&) {
      return std::nullopt;
    }
  }
} // namespace

LyricsService::LyricsService(HttpClient& httpClient, const ConfigService& config)
    : m_httpClient(httpClient), m_config(config) {}

bool LyricsService::staleLengthFor(const MprisPlayerInfo& player, const std::string& key) {
  auto& last = m_lastLengths[player.busName];
  if (last.trackKey != key) {
    last.stale = player.lengthUs > 0 && player.lengthUs == last.lengthUs;
    last.trackKey = key;
  } else if (player.lengthUs != last.lengthUs) {
    last.stale = false;
  }
  last.lengthUs = player.lengthUs;
  if (last.stale) {
    kLog.debug("ignoring track length repeated from the previous track");
  }
  return last.stale;
}

bool LyricsService::offline() const { return m_config.config().shell.offlineMode; }

bool LyricsService::continuesTrack(
    const MprisPlayerInfo& player, std::string_view trackId, std::string_view title, std::int64_t lengthUs,
    std::optional<std::int64_t> expectedPositionUs
) {
  if (player.trackId != trackId) {
    return false;
  }
  if (lyricsTitleContinues(title, player.title)) {
    return true;
  }
  const bool sameLength = lengthUs > 0 && player.lengthUs == lengthUs;
  if (sameLength && player.playbackStatus == "Paused") {
    return true;
  }
  return sameLength
      && player.playbackStatus == "Playing"
      && expectedPositionUs
      && std::llabs(player.positionUs - *expectedPositionUs) <= kPositionContinuityUs;
}

std::string LyricsService::trackKey(const MprisPlayerInfo& player) {
  return nlohmann::json::array({player.busName, player.trackId, player.title, player.artists}).dump();
}

int LyricsService::sourceOrder(Provider provider) {
  switch (provider) {
  case Provider::LRCMUX:
    return 0;
  case Provider::LYRICSPLUS:
    return 1;
  case Provider::LRCLIB:
    return 2;
  case Provider::KUGOU_LYRIC:
    return 3;
  case Provider::NETEASE_LYRIC:
    return 4;
  default:
    return 5;
  }
}

std::size_t LyricsService::throttleSlot(Provider provider) {
  switch (provider) {
  case Provider::MUSICBRAINZ:
    return 0;
  case Provider::DEEZER_SEARCH:
  case Provider::DEEZER_TRACK:
    return 1;
  case Provider::LRCMUX:
    return 2;
  case Provider::NETEASE_SEARCH:
  case Provider::NETEASE_LYRIC:
    return 3;
  case Provider::LRCLIB:
    return 4;
  case Provider::LYRICSPLUS:
    return 5;
  case Provider::KUGOU_SONG:
  case Provider::KUGOU_SEARCH:
  case Provider::KUGOU_LYRIC:
    return 6;
  }
  std::unreachable();
}

void LyricsService::request(const MprisPlayerInfo& player, Callback callback) {
  if (auto local = sidecarLyrics(player)) {
    DeferredCall::callLater([callback = std::move(callback), local = std::move(local)] { callback(local); });
    return;
  }
  if (offline()) {
    auto result = std::make_shared<LyricsResult>();
    result->status = LyricsStatus::OFFLINE;
    DeferredCall::callLater([callback = std::move(callback), result] { callback(result); });
    return;
  }
  const auto key = trackKey(player);
  const bool staleLength = staleLengthFor(player, key);
  // A title that wraps the last found one (e.g. a scraped page title) is the same track. Weaker evidence needs the
  // playback history only the caller has, so it is not applied here.
  if (const auto last = m_lastReady.find(player.busName); last != m_lastReady.end()
      && last->second.key != key
      && last->second.trackId == player.trackId
      && lyricsTitleContinues(last->second.title, player.title)) {
    if (const auto kept = m_cache.find(last->second.key); kept != m_cache.end()
        && kept->second.result
        && kept->second.result->status == LyricsStatus::READY
        && kept->second.expiresAt > std::chrono::steady_clock::now()) {
      DeferredCall::callLater([callback = std::move(callback), result = kept->second.result] { callback(result); });
      return;
    }
  }
  auto existing = m_cache.find(key);
  if (existing != m_cache.end() && existing->second.result) {
    const bool metadataChanged = meaningfulMetadataChanged(existing->second.rawTrack, lookupTrack(player, staleLength));
    const bool retryWithMetadata = existing->second.result->status != LyricsStatus::READY && metadataChanged;
    if (retryWithMetadata || existing->second.expiresAt <= std::chrono::steady_clock::now()) {
      m_cache.erase(existing);
      existing = m_cache.end();
    }
  }
  if (existing != m_cache.end()) {
    if (existing->second.result) {
      DeferredCall::callLater([callback = std::move(callback), result = existing->second.result] { callback(result); });
    } else {
      const auto current = lookupTrack(player, staleLength);
      const auto& previous = existing->second.pendingTrack ? *existing->second.pendingTrack : existing->second.rawTrack;
      if (meaningfulMetadataChanged(previous, current)) {
        existing->second.pendingTrack = current;
      }
      existing->second.priority = ++m_priorityCounter;
      existing->second.callbacks.push_back(std::move(callback));
    }
    return;
  }
  Entry entry;
  entry.busName = player.busName;
  entry.trackId = player.trackId;
  entry.rawTrack = lookupTrack(player, staleLength);
  entry.track = entry.rawTrack;
  entry.priority = ++m_priorityCounter;
  entry.callbacks.push_back(std::move(callback));
  m_cache.emplace(key, std::move(entry));
  requestCatalog(key);
}

void LyricsService::enqueue(const std::string& key, Provider provider, std::string url) {
  auto& entry = m_cache.at(key);
  entry.provider = provider;
  entry.url = std::move(url);
  m_queue.push_back(key);
  startNext();
}

void LyricsService::requestCatalog(const std::string& key) {
  const auto query = lyricsSearchText(m_cache.at(key).rawTrack);
  if (query.empty()) {
    requestMusicBrainz(key);
    return;
  }
  enqueue(key, Provider::DEEZER_SEARCH, "https://api.deezer.com/search?limit=25&q=" + uri::encodeComponent(query));
}

void LyricsService::requestMusicBrainz(const std::string& key) {
  enqueue(
      key, Provider::MUSICBRAINZ,
      "https://musicbrainz.org/ws/2/recording/?fmt=json&limit=25&query="
          + uri::encodeComponent(lyricsCatalogQuery(m_cache.at(key).rawTrack))
  );
}

void LyricsService::requestLyrics(const std::string& key) {
  const auto& entry = m_cache.at(key);
  std::string url = "https://api.lrcmux.dev/get?format=json&level=word";
  if (!entry.isrc.empty()) {
    url += "&isrc=" + uri::encodeComponent(entry.isrc);
  } else {
    url += "&title=" + uri::encodeComponent(entry.track.title) + "&artist=" + uri::encodeComponent(entry.track.artist);
    if (!entry.track.album.empty()) {
      url += "&album=" + uri::encodeComponent(entry.track.album);
    }
    if (entry.track.durationUs > 0) {
      const auto seconds = entry.track.durationUs / 1000000 + (entry.track.durationUs % 1000000 >= 500000 ? 1 : 0);
      url += std::format("&duration={}", seconds);
    }
  }
  enqueue(key, Provider::LRCMUX, std::move(url));
}

void LyricsService::startNext() {
  if (m_busy || m_queue.empty()) {
    return;
  }
  if (offline()) {
    auto pending = std::move(m_queue);
    m_queue.clear();
    for (const auto& key : pending) {
      auto result = std::make_shared<LyricsResult>();
      result->status = LyricsStatus::OFFLINE;
      complete(key, std::move(result));
    }
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  const auto slotAt = [this](const std::string& key) {
    return m_nextRequestAt[throttleSlot(m_cache.at(key).provider)];
  };
  // Serve the most recently requested track first so stale lookups from earlier metadata never delay it.
  auto next = m_queue.end();
  for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
    if (slotAt(*it) <= now && (next == m_queue.end() || m_cache.at(*it).priority > m_cache.at(*next).priority)) {
      next = it;
    }
  }
  if (next == m_queue.end()) {
    const auto earliest = std::ranges::min(m_queue | std::views::transform(slotAt));
    m_requestTimer.start(std::chrono::ceil<std::chrono::milliseconds>(earliest - now), [this] { startNext(); });
    return;
  }
  auto key = std::move(*next);
  m_queue.erase(next);
  m_busy = true;
  HttpRequest request;
  request.url = m_cache.at(key).url;
  request.headers = {std::format("User-Agent: Noctalia/{} (https://noctalia.dev)", noctalia::build_info::version())};
  const std::weak_ptr<void> guard = m_aliveGuard;
  m_httpClient.request(std::move(request), [this, guard, key](HttpResponse response) {
    if (!guard.expired()) {
      finish(key, std::move(response));
    }
  });
}

void LyricsService::finish(const std::string& key, HttpResponse response) {
  auto& entry = m_cache.at(key);
  const auto providerName = kProviderNames.at(static_cast<std::size_t>(entry.provider));
  kLog.debug("response provider={} transport_ok={} status={}", providerName, response.transportOk, response.status);
  const auto now = std::chrono::steady_clock::now();
  auto& nextRequestAt = m_nextRequestAt[throttleSlot(entry.provider)];
  nextRequestAt = now + (entry.provider == Provider::MUSICBRAINZ ? kCatalogSpacing : kRequestSpacing);
  const bool responseOk = response.transportOk && response.status == 200;
  if (!responseOk && (!response.transportOk || response.status != 404)) {
    entry.hadError = true;
  }
  if (response.status == 429 || response.status == 503) {
    std::int64_t seconds = kErrorLifetime.count();
    if (const auto it = response.headers.find("retry-after"); it != response.headers.end()) {
      std::int64_t parsed = 0;
      const auto [end, error] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), parsed);
      const auto available =
          std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::time_point::max() - now).count();
      if (error == std::errc{} && end == it->second.data() + it->second.size() && parsed > 0) {
        seconds = std::min(parsed, available);
      }
    }
    nextRequestAt = now + std::chrono::seconds{seconds};
  }
  m_busy = false;
  if (offline()) {
    auto result = std::make_shared<LyricsResult>();
    result->status = LyricsStatus::OFFLINE;
    complete(key, std::move(result));
    return;
  }
  if (entry.provider == Provider::MUSICBRAINZ || entry.provider == Provider::DEEZER_SEARCH) {
    if (responseOk) {
      const auto matched = entry.provider == Provider::MUSICBRAINZ
          ? matchLyricsRecording(response.body, entry.rawTrack)
          : matchDeezerRecording(response.body, entry.rawTrack);
      kLog.debug(
          "catalog provider={} valid={} matched={}", providerName, matched.has_value(),
          matched.has_value() && matched->has_value()
      );
      if (!matched) {
        entry.hadError = true;
      } else if (*matched) {
        entry.track = (**matched).track;
        entry.isrc = (**matched).isrc;
        entry.deezerId = (**matched).deezerId;
        if (entry.isrc.empty() && entry.deezerId != 0) {
          enqueue(key, Provider::DEEZER_TRACK, std::format("https://api.deezer.com/track/{}", entry.deezerId));
        } else {
          requestLyrics(key);
        }
        return;
      }
    }
    if (entry.provider == Provider::DEEZER_SEARCH) {
      requestMusicBrainz(key);
    } else {
      requestLyrics(key);
    }
    return;
  }
  if (entry.provider == Provider::DEEZER_TRACK) {
    if (responseOk) {
      if (auto isrc = deezerIsrc(response.body, entry.deezerId)) {
        entry.isrc = std::move(*isrc);
      }
    }
    requestLyrics(key);
    return;
  }
  if (!response.transportOk) {
    const auto slot = throttleSlot(entry.provider);
    if (slot == throttleSlot(Provider::LYRICSPLUS)
        || slot == throttleSlot(Provider::KUGOU_SONG)
        || slot == throttleSlot(Provider::NETEASE_SEARCH)) {
      m_backoffUntil[slot] = now + kProviderBackoff;
    }
  }
  if (entry.provider == Provider::KUGOU_SONG || entry.provider == Provider::NETEASE_SEARCH) {
    const bool kugou = entry.provider == Provider::KUGOU_SONG;
    if (responseOk) {
      const auto song =
          kugou ? matchKugouSong(response.body, entry.track) : matchNeteaseSong(response.body, entry.track);
      kLog.debug(
          "catalog provider={} valid={} matched={}", providerName, song.has_value(),
          song.has_value() && song->has_value()
      );
      if (!song) {
        entry.hadError = true;
      } else if (*song) {
        entry.sourceDurationMs = (**song).track.durationUs / 1000;
        if (kugou) {
          enqueue(
              key, Provider::KUGOU_SEARCH,
              "https://lyrics.kugou.com/search?ver=1&man=yes&client=pc&hash=" + uri::encodeComponent((**song).sourceId)
          );
        } else {
          enqueue(
              key, Provider::NETEASE_LYRIC,
              std::format("https://music.163.com/api/song/lyric/v1?id={}&lv=1&yv=1&tv=-1", (**song).sourceId)
          );
        }
        return;
      }
    }
    if (kugou) {
      requestNetease(key);
    } else {
      finishBest(key);
    }
    return;
  }
  if (entry.provider == Provider::KUGOU_SEARCH) {
    if (responseOk) {
      const auto lyrics = pickKugouLyrics(response.body);
      if (!lyrics) {
        entry.hadError = true;
      } else if (*lyrics) {
        enqueue(
            key, Provider::KUGOU_LYRIC,
            "https://lyrics.kugou.com/download?ver=1&client=pc&charset=utf8&fmt=lrc&id="
                + uri::encodeComponent((**lyrics).id)
                + "&accesskey="
                + uri::encodeComponent((**lyrics).accessKey)
        );
        return;
      }
    }
    requestNetease(key);
    return;
  }
  auto result = std::make_shared<LyricsResult>();
  if (responseOk) {
    switch (entry.provider) {
    case Provider::LRCMUX:
      *result = parseLrcmuxResponse(response.body, entry.track, entry.isrc);
      break;
    case Provider::LYRICSPLUS:
      *result = parseLyricsPlusResponse(response.body, entry.track);
      break;
    case Provider::KUGOU_LYRIC:
      *result = parseKugouLyric(response.body);
      break;
    case Provider::NETEASE_LYRIC:
      *result = parseNeteaseLyric(response.body);
      break;
    default:
      *result = parseLrclibSearch(response.body, entry.track);
      break;
    }
    if (result->status == LyricsStatus::ERROR) {
      entry.hadError = true;
    }
    if (result->recordingDurationMs <= 0
        && (entry.provider == Provider::KUGOU_LYRIC || entry.provider == Provider::NETEASE_LYRIC)) {
      result->recordingDurationMs = entry.sourceDurationMs;
    }
    kLog.debug(
        "parsed provider={} result={} lines={}", providerName, static_cast<int>(result->status),
        result->document.lines.size()
    );
  }
  if (result->status == LyricsStatus::READY && result->document.instrumental) {
    complete(key, std::move(result));
    return;
  }
  consider(entry, std::move(result));
  if (settled(entry)) {
    finishBest(key);
    return;
  }
  switch (entry.provider) {
  case Provider::LRCMUX:
    requestLyricsPlus(key);
    break;
  case Provider::LYRICSPLUS:
    requestLrclib(key);
    break;
  case Provider::LRCLIB:
    // KuGou and NetEase are a last resort: only asked when no other source has synced lyrics.
    if (entry.best && lyricsQuality(*entry.best) >= 2) {
      finishBest(key);
    } else {
      requestKugou(key);
    }
    break;
  case Provider::KUGOU_LYRIC:
    requestNetease(key);
    break;
  default:
    finishBest(key);
    break;
  }
}

std::string LyricsService::sourceQuery(const Entry& entry) const {
  const bool catalogMatched = !entry.isrc.empty() || entry.deezerId != 0;
  return catalogMatched ? entry.track.title + " " + entry.track.artist : lyricsSearchText(entry.rawTrack);
}

void LyricsService::requestLyricsPlus(const std::string& key) {
  const auto& entry = m_cache.at(key);
  if (std::chrono::steady_clock::now() < m_backoffUntil[throttleSlot(Provider::LYRICSPLUS)]) {
    requestLrclib(key);
    return;
  }
  std::string url = "https://lyricsplus.binimum.org/v2/lyrics/get?title="
      + uri::encodeComponent(entry.track.title)
      + "&artist="
      + uri::encodeComponent(entry.track.artist);
  if (!entry.track.album.empty()) {
    url += "&album=" + uri::encodeComponent(entry.track.album);
  }
  if (entry.track.durationUs > 0) {
    url += std::format("&duration={}", (entry.track.durationUs + 500000) / 1000000);
  }
  enqueue(key, Provider::LYRICSPLUS, std::move(url));
}

void LyricsService::requestLrclib(const std::string& key) {
  enqueue(
      key, Provider::LRCLIB,
      "https://lrclib.net/api/search?q=" + uri::encodeComponent(lyricsSearchText(m_cache.at(key).track))
  );
}

void LyricsService::requestKugou(const std::string& key) {
  auto& entry = m_cache.at(key);
  if (std::chrono::steady_clock::now() < m_backoffUntil[throttleSlot(Provider::KUGOU_SONG)]) {
    requestNetease(key);
    return;
  }
  entry.sourceDurationMs = 0;
  enqueue(
      key, Provider::KUGOU_SONG,
      "https://mobileservice.kugou.com/api/v3/search/song?version=9108&plat=0&pagesize=8&showtype=0&keyword="
          + uri::encodeComponent(sourceQuery(entry))
  );
}

void LyricsService::requestNetease(const std::string& key) {
  auto& entry = m_cache.at(key);
  if (std::chrono::steady_clock::now() < m_backoffUntil[throttleSlot(Provider::NETEASE_SEARCH)]) {
    finishBest(key);
    return;
  }
  entry.sourceDurationMs = 0;
  enqueue(
      key, Provider::NETEASE_SEARCH,
      "https://music.163.com/api/search/get?type=1&limit=10&s=" + uri::encodeComponent(sourceQuery(entry))
  );
}

void LyricsService::consider(Entry& entry, std::shared_ptr<const LyricsResult> result) {
  const auto wantedMs = entry.rawTrack.durationUs / 1000;
  const auto rank = [wantedMs](const LyricsResult& lyrics, Provider provider) {
    const int quality = lyricsQuality(lyrics);
    return std::tuple{
        lengthMatches(lyrics, wantedMs) && quality >= 2, quality >= 2, quality >= 1, -sourceOrder(provider)
    };
  };
  if (lyricsQuality(*result) == 0) {
    return;
  }
  if (!entry.best || rank(*result, entry.provider) > rank(*entry.best, entry.bestProvider)) {
    entry.best = std::move(result);
    entry.bestProvider = entry.provider;
  }
}

bool LyricsService::settled(const Entry& entry) const {
  if (!entry.best || lyricsQuality(*entry.best) < 2) {
    return false;
  }
  const auto wantedMs = entry.rawTrack.durationUs / 1000;
  return wantedMs <= 0 || lengthMatches(*entry.best, wantedMs);
}

void LyricsService::finishBest(const std::string& key) {
  auto& entry = m_cache.at(key);
  auto result = std::make_shared<LyricsResult>();
  if (entry.best) {
    *result = *entry.best;
  } else if (entry.hadError) {
    result->status = LyricsStatus::ERROR;
  }
  complete(key, std::move(result));
}

void LyricsService::complete(const std::string& key, const std::shared_ptr<const LyricsResult>& result) {
  auto& entry = m_cache.at(key);
  if (result->status != LyricsStatus::READY && entry.pendingTrack && !offline()) {
    entry.rawTrack = std::move(*entry.pendingTrack);
    entry.track = entry.rawTrack;
    entry.pendingTrack.reset();
    entry.isrc.clear();
    entry.deezerId = 0;
    entry.hadError = false;
    entry.best.reset();
    kLog.debug("restarting lookup with completed metadata");
    requestCatalog(key);
    return;
  }
  entry.pendingTrack.reset();
  const auto now = std::chrono::steady_clock::now();
  entry.result = result;
  if (result->status == LyricsStatus::READY) {
    m_lastReady[entry.busName] = {.key = key, .trackId = entry.trackId, .title = entry.rawTrack.title};
  }
  entry.expiresAt = result->status == LyricsStatus::ERROR || result->status == LyricsStatus::OFFLINE
      ? now + kErrorLifetime
      : result->status == LyricsStatus::NOT_FOUND ? now + std::chrono::minutes{5}
                                                  : now + std::chrono::hours{1};
  auto callbacks = std::move(entry.callbacks);
  for (const auto& callback : callbacks) {
    callback(result);
  }
  trimCache();
  startNext();
}

void LyricsService::trimCache() {
  while (m_cache.size() > kMaxCacheEntries) {
    auto oldest = m_cache.end();
    for (auto it = m_cache.begin(); it != m_cache.end(); ++it) {
      if (it->second.result && (oldest == m_cache.end() || it->second.expiresAt < oldest->second.expiresAt)) {
        oldest = it;
      }
    }
    if (oldest == m_cache.end()) {
      break;
    }
    m_cache.erase(oldest);
  }
}
