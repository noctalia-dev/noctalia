#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

struct MprisPlayerInfo;
class HttpClient;

namespace mpris {

  bool isRemoteArtUrl(std::string_view url);
  std::string effectiveArtUrl(const MprisPlayerInfo& player);
  // Ordered download URLs for an effective art URL: the primary plus any lower-res
  // fallback (e.g. i.ytimg maxresdefault → hqdefault) tried when the primary 404s.
  std::vector<std::string> artFetchCandidates(std::string_view primaryUrl);

  // Local file to decode for artUrl right now: the file path for file URLs, or the
  // cached download for remote URLs. Empty when a remote URL isn't cached yet. Never
  // starts a download.
  std::string cachedArtworkPath(std::string_view artUrl);
  // Like cachedArtworkPath, but for an uncached remote URL kicks off a deduped
  // background download (see artFetchCandidates) keyed in `pending`; `onReady` runs
  // when a download lands. Returns empty until the file exists. `lifetime` guards the
  // deferred completion: `pending` and `onReady` are touched only while it is alive, so
  // the owner of `pending`/`onReady` may be destroyed before the download lands.
  std::string resolveArtworkSource(
      HttpClient* httpClient, std::unordered_set<std::string>& pending, std::string_view artUrl,
      std::function<void()> onReady, std::weak_ptr<void> lifetime
  );
  std::string normalizeArtPath(std::string_view artUrl);
  std::filesystem::path artCachePath(std::string_view artUrl);
  std::string joinArtists(const std::vector<std::string>& artists);

} // namespace mpris
