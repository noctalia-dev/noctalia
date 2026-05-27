#include "dbus/mpris/mpris_art.h"

#include "dbus/mpris/mpris_service.h"
#include "net/uri.h"

#include <format>
#include <string_view>

namespace {

  std::string extractQueryParam(std::string_view url, std::string_view key) {
    const auto queryPos = url.find('?');
    if (queryPos == std::string_view::npos) {
      return {};
    }
    std::string_view query = url.substr(queryPos + 1);
    while (!query.empty()) {
      const auto ampPos = query.find('&');
      const std::string_view pair = query.substr(0, ampPos);
      const auto eqPos = pair.find('=');
      if (pair.substr(0, eqPos) == key) {
        return eqPos == std::string_view::npos ? std::string{} : std::string(pair.substr(eqPos + 1));
      }
      if (ampPos == std::string_view::npos) {
        break;
      }
      query.remove_prefix(ampPos + 1);
    }
    return {};
  }

  std::string deriveYouTubeThumbnailUrl(std::string_view sourceUrl) {
    if (sourceUrl.empty()) {
      return {};
    }
    std::string videoId;
    if (sourceUrl.find("youtube.com/watch") != std::string_view::npos) {
      videoId = extractQueryParam(sourceUrl, "v");
    } else if (sourceUrl.find("youtu.be/") != std::string_view::npos) {
      const auto marker = sourceUrl.find("youtu.be/");
      const auto start = marker + std::string_view("youtu.be/").size();
      const auto end = sourceUrl.find_first_of("?#&/", start);
      videoId =
          std::string(sourceUrl.substr(start, end == std::string_view::npos ? sourceUrl.size() - start : end - start));
    } else if (sourceUrl.find("youtube.com/shorts/") != std::string_view::npos) {
      const auto marker = sourceUrl.find("youtube.com/shorts/");
      const auto start = marker + std::string_view("youtube.com/shorts/").size();
      const auto end = sourceUrl.find_first_of("?#&/", start);
      videoId =
          std::string(sourceUrl.substr(start, end == std::string_view::npos ? sourceUrl.size() - start : end - start));
    }
    if (videoId.empty()) {
      return {};
    }
    return std::format("https://i.ytimg.com/vi/{}/hqdefault.jpg", videoId);
  }

} // namespace

namespace mpris {

  bool isRemoteArtUrl(std::string_view url) { return uri::isRemoteUrl(url); }

  std::string effectiveArtUrl(const MprisPlayerInfo& player) {
    if (!player.artUrl.empty()) {
      return player.artUrl;
    }
    return deriveYouTubeThumbnailUrl(player.sourceUrl);
  }

  std::string normalizeArtPath(std::string_view artUrl) { return uri::normalizeFileUrl(artUrl); }

  std::filesystem::path artCachePath(std::string_view artUrl) {
    const std::filesystem::path cacheDir = std::filesystem::path("/tmp") / "noctalia-media-art";
    const std::size_t hash = std::hash<std::string_view>{}(artUrl);
    return cacheDir / (std::to_string(hash) + ".img");
  }

  std::string joinArtists(const std::vector<std::string>& artists) {
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

} // namespace mpris
