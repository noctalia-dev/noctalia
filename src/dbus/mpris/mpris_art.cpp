#include "dbus/mpris/mpris_art.h"

#include "dbus/mpris/mpris_service.h"
#include "net/http_client.h"
#include "net/uri.h"

#include <format>
#include <string_view>

namespace {

  constexpr std::string_view kGoogleArtSizeSuffix = "=w544-h544-l90-rj";

  std::string extractQueryParam(std::string_view url, std::string_view key) {
    const auto queryPos = url.find('?');
    if (queryPos == std::string_view::npos)
      return {};
    std::string_view query = url.substr(queryPos + 1);
    while (!query.empty()) {
      const auto ampPos = query.find('&');
      const std::string_view pair = query.substr(0, ampPos);
      const auto eqPos = pair.find('=');
      if (pair.substr(0, eqPos) == key)
        return eqPos == std::string_view::npos ? std::string{} : std::string(pair.substr(eqPos + 1));
      if (ampPos == std::string_view::npos)
        break;
      query.remove_prefix(ampPos + 1);
    }
    return {};
  }

  // Empty if `key` is not present. Used so cover APIs that 500 on size=N (common
  // for Subsonic/Navidrome WebP) can fall back to the original bitstream.
  [[nodiscard]] std::string withoutQueryParam(std::string_view url, std::string_view key) {
    const auto queryPos = url.find('?');
    if (queryPos == std::string_view::npos)
      return {};
    std::string out(url.substr(0, queryPos + 1));
    std::string_view query = url.substr(queryPos + 1);
    bool anyKept = false;
    bool stripped = false;
    while (!query.empty()) {
      const auto ampPos = query.find('&');
      const std::string_view pair = query.substr(0, ampPos);
      const auto eqPos = pair.find('=');
      const std::string_view k = pair.substr(0, eqPos);
      if (k == key) {
        stripped = true;
      } else {
        if (anyKept)
          out.push_back('&');
        out.append(pair);
        anyKept = true;
      }
      if (ampPos == std::string_view::npos)
        break;
      query.remove_prefix(ampPos + 1);
    }
    if (!stripped)
      return {};
    if (!anyKept) {
      out.pop_back();
    }
    return out;
  }

  std::string deriveYouTubeThumbnailUrl(std::string_view sourceUrl, std::string_view quality) {
    if (sourceUrl.empty())
      return {};
    std::string videoId;
    if (sourceUrl.contains("youtube.com/watch") || sourceUrl.contains("music.youtube.com/watch")) {
      videoId = extractQueryParam(sourceUrl, "v");
    } else if (sourceUrl.contains("youtu.be/")) {
      const auto marker = sourceUrl.find("youtu.be/");
      const auto start = marker + std::string_view("youtu.be/").size();
      const auto end = sourceUrl.find_first_of("?#&/", start);
      videoId =
          std::string(sourceUrl.substr(start, end == std::string_view::npos ? sourceUrl.size() - start : end - start));
    } else if (sourceUrl.contains("youtube.com/shorts/")) {
      const auto marker = sourceUrl.find("youtube.com/shorts/");
      const auto start = marker + std::string_view("youtube.com/shorts/").size();
      const auto end = sourceUrl.find_first_of("?#&/", start);
      videoId =
          std::string(sourceUrl.substr(start, end == std::string_view::npos ? sourceUrl.size() - start : end - start));
    }
    if (videoId.empty())
      return {};
    return std::format("https://i.ytimg.com/vi/{}/{}.jpg", videoId, quality);
  }

  [[nodiscard]] std::string upgradeGoogleArtUrl(std::string_view url) {
    if (!url.contains("googleusercontent.com") && !url.contains("ggpht.com")) {
      return std::string(url);
    }

    const auto paramStart = url.rfind('=');
    if (paramStart == std::string_view::npos) {
      return std::string(url);
    }

    std::string upgraded(url.substr(0, paramStart));
    upgraded.append(kGoogleArtSizeSuffix);
    return upgraded;
  }

  [[nodiscard]] bool isYouTubeMusicSourceUrl(std::string_view sourceUrl) {
    return sourceUrl.contains("music.youtube.com");
  }

} // namespace

namespace mpris {

  bool isRemoteArtUrl(std::string_view url) { return uri::isRemoteUrl(url); }

  std::string effectiveArtUrl(const MprisPlayerInfo& player) {
    // YouTube Music advertises a tiny Google-CDN album-art URL; bumping its size
    // suffix only upscales a small source. The per-video i.ytimg thumbnail is the
    // same album cover at far higher resolution, so prefer it when derivable.
    if (isYouTubeMusicSourceUrl(player.sourceUrl)) {
      std::string derived = deriveYouTubeThumbnailUrl(player.sourceUrl, "maxresdefault");
      if (!derived.empty())
        return derived;
    }
    if (!player.artUrl.empty()) {
      if (isRemoteArtUrl(player.artUrl)) {
        return upgradeGoogleArtUrl(player.artUrl);
      }
      return player.artUrl;
    }
    return deriveYouTubeThumbnailUrl(player.sourceUrl, "hqdefault");
  }

  std::vector<std::string> artFetchCandidates(std::string_view primaryUrl) {
    std::vector<std::string> candidates;
    if (primaryUrl.empty())
      return candidates;
    candidates.emplace_back(primaryUrl);
    // maxresdefault is not generated for every video; hqdefault always exists.
    constexpr std::string_view kMaxres = "/maxresdefault.jpg";
    if (primaryUrl.contains("i.ytimg.com/vi/")
        && primaryUrl.size() >= kMaxres.size()
        && primaryUrl.substr(primaryUrl.size() - kMaxres.size()) == kMaxres) {
      std::string hq(primaryUrl.substr(0, primaryUrl.size() - kMaxres.size()));
      hq += "/hqdefault.jpg";
      candidates.push_back(std::move(hq));
    }
    if (std::string stripped = withoutQueryParam(primaryUrl, "size"); !stripped.empty()) {
      candidates.push_back(std::move(stripped));
    }
    return candidates;
  }

  std::string cachedArtworkPath(std::string_view artUrl) {
    if (artUrl.empty())
      return {};
    std::string local = normalizeArtPath(artUrl);
    if (!local.empty())
      return local;
    if (!isRemoteArtUrl(artUrl))
      return {};
    const auto cached = artCachePath(artUrl);
    std::error_code ec;
    if (std::filesystem::exists(cached, ec) && std::filesystem::file_size(cached, ec) > 0)
      return cached.string();
    return {};
  }

  std::string resolveArtworkSource(
      HttpClient* httpClient, std::unordered_set<std::string>& pending, std::string_view artUrl,
      std::function<void()> onReady, std::weak_ptr<void> lifetime
  ) {
    std::string path = cachedArtworkPath(artUrl);
    if (!path.empty() || !isRemoteArtUrl(artUrl) || httpClient == nullptr)
      return path;
    const std::string key(artUrl);
    if (!pending.insert(key).second)
      return {};
    const auto cached = artCachePath(artUrl);
    std::error_code ec;
    std::filesystem::create_directories(cached.parent_path(), ec);
    httpClient->download(
        artFetchCandidates(artUrl), cached,
        [&pending, key, onReady = std::move(onReady), lifetime = std::move(lifetime)](bool success) {
          if (lifetime.expired())
            return;
          pending.erase(key);
          if (success && onReady)
            onReady();
        }
    );
    return {};
  }

  std::string normalizeArtPath(std::string_view artUrl) { return uri::normalizeFileUrl(artUrl); }

  std::filesystem::path artCachePath(std::string_view artUrl) {
    const std::filesystem::path cacheDir = std::filesystem::path("/tmp") / "noctalia-media-art";
    const std::size_t hash = std::hash<std::string_view>{}(artUrl);
    return cacheDir / (std::to_string(hash) + ".img");
  }

  std::string joinArtists(const std::vector<std::string>& artists) {
    if (artists.empty())
      return {};
    std::string joined = artists.front();
    for (std::size_t i = 1; i < artists.size(); ++i) {
      joined += ", ";
      joined += artists[i];
    }
    return joined;
  }

} // namespace mpris
