#include "theme/community_palettes.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "net/http_client.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace noctalia::theme {

  namespace {

    constexpr Logger kLog("community_palettes");
    constexpr std::string_view kCatalogUrl = "https://api.noctalia.dev/palettes";
    constexpr std::string_view kPaletteUrlBase = "https://api.noctalia.dev/palette";

    std::filesystem::path catalogCachePath() { return communityPaletteCacheDir() / ".catalog" / "palettes.json"; }

    std::string stringField(const nlohmann::json& obj, std::string_view key) {
      auto it = obj.find(std::string(key));
      if (it == obj.end() || !it->is_string()) {
        return {};
      }
      return it->get<std::string>();
    }

    std::string paletteNameFromJson(const nlohmann::json& item) {
      if (item.is_string()) {
        return item.get<std::string>();
      }
      if (!item.is_object()) {
        return {};
      }
      return stringField(item, "name");
    }

    AvailablePalette::PreviewMode palettePreviewFromJson(const nlohmann::json& item, std::string_view mode) {
      if (!item.is_object()) {
        return {};
      }
      auto modeIt = item.find(std::string(mode));
      if (modeIt == item.end() || !modeIt->is_object()) {
        return {};
      }

      static constexpr auto kPreviewFields = std::to_array<std::string_view>({
          "primary",
          "secondary",
          "tertiary",
          "error",
      });

      AvailablePalette::PreviewMode preview;
      preview.accents.reserve(kPreviewFields.size());
      preview.surface = stringField(*modeIt, "surface");
      for (const auto field : kPreviewFields) {
        std::string color = stringField(*modeIt, field);
        if (!color.empty()) {
          preview.accents.push_back(std::move(color));
        }
      }
      return preview;
    }

    std::vector<AvailablePalette> parseCatalogFile(const std::filesystem::path& path) {
      std::ifstream in(path);
      if (!in) {
        return {};
      }

      try {
        std::stringstream buf;
        buf << in.rdbuf();
        const auto root = nlohmann::json::parse(buf.str());
        const nlohmann::json* entries = &root;
        if (root.is_object()) {
          if (auto it = root.find("palettes"); it != root.end()) {
            entries = &*it;
          }
        }
        if (!entries->is_array()) {
          return {};
        }

        std::vector<AvailablePalette> out;
        out.reserve(entries->size());
        for (const auto& item : *entries) {
          AvailablePalette palette;
          palette.name = paletteNameFromJson(item);
          palette.preview.dark = palettePreviewFromJson(item, "dark");
          palette.preview.light = palettePreviewFromJson(item, "light");
          if (!palette.name.empty()) {
            out.push_back(std::move(palette));
          }
        }
        std::sort(out.begin(), out.end(), [](const AvailablePalette& a, const AvailablePalette& b) {
          return a.name < b.name;
        });
        out.erase(
            std::unique(
                out.begin(), out.end(),
                [](const AvailablePalette& a, const AvailablePalette& b) { return a.name == b.name; }
            ),
            out.end()
        );
        return out;
      } catch (const std::exception& e) {
        kLog.warn("failed to parse community palette catalog {}: {}", path.string(), e.what());
        return {};
      }
    }

  } // namespace

  CommunityPaletteService::CommunityPaletteService(HttpClient& httpClient) : m_httpClient(httpClient) {}

  void CommunityPaletteService::setReadyCallback(ReadyCallback callback) { m_readyCallback = std::move(callback); }

  void CommunityPaletteService::sync() {
    const std::uint64_t generation = ++m_generation;
    const std::filesystem::path path = catalogCachePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    m_httpClient.download(kCatalogUrl, path, [this, generation, path](bool success) {
      if (generation != m_generation) {
        return;
      }
      if (!success) {
        kLog.warn("failed to refresh community palette catalog; using cached metadata when available");
        return;
      }
      if (parseCatalogFile(path).empty()) {
        kLog.warn("community palette catalog downloaded but contained no palettes");
      }
      if (m_readyCallback) {
        DeferredCall::callLater([callback = m_readyCallback]() { callback(); });
      }
    });
  }

  std::vector<AvailablePalette> availableCommunityPalettes() { return parseCatalogFile(catalogCachePath()); }

  std::filesystem::path communityPaletteCacheDir() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] != '\0') {
      return std::filesystem::path(xdg) / "noctalia" / "community-palettes";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      return std::filesystem::path(home) / ".cache" / "noctalia" / "community-palettes";
    }
    return std::filesystem::path("/tmp") / "noctalia" / "community-palettes";
  }

  std::filesystem::path communityPaletteCachePath(std::string_view name) {
    return communityPaletteCacheDir() / (StringUtils::urlEncode(name) + ".json");
  }

  std::string communityPaletteDownloadUrl(std::string_view name) {
    return std::string(kPaletteUrlBase) + "/" + StringUtils::urlEncode(name);
  }

} // namespace noctalia::theme
