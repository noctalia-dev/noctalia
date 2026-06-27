#include "launcher/wallpaper_provider.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "shell/wallpaper/wallpaper_paths.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <vector>

namespace {

  constexpr std::size_t kMaxResults = 50;

  struct WallpaperCandidate {
    std::string name;
    std::string path;
    std::string searchable;
  };

  bool hasImageExtension(const std::filesystem::path& path) {
    const auto ext = StringUtils::toLower(path.extension().string());
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".webp" || ext == ".bmp" || ext == ".gif";
  }

  std::vector<WallpaperCandidate> collectWallpapers(const std::filesystem::path& directory) {
    std::vector<WallpaperCandidate> candidates;
    if (directory.empty()) {
      return candidates;
    }

    std::error_code ec;
    if (!std::filesystem::exists(directory, ec) || !std::filesystem::is_directory(directory, ec)) {
      return candidates;
    }

    for (auto it = std::filesystem::recursive_directory_iterator(
             directory, std::filesystem::directory_options::skip_permission_denied, ec
         );
         !ec && it != std::filesystem::end(it); it.increment(ec)) {
      if (ec) {
        break;
      }

      std::error_code typeEc;
      if (!it->is_regular_file(typeEc) || typeEc || !hasImageExtension(it->path())) {
        continue;
      }

      WallpaperCandidate candidate;
      candidate.name = it->path().filename().string();
      candidate.path = it->path().string();
      candidate.searchable = StringUtils::toLower(candidate.name + " " + it->path().parent_path().filename().string());
      candidates.push_back(std::move(candidate));
    }

    std::ranges::sort(candidates, [](const auto& a, const auto& b) {
      return StringUtils::toLower(a.name) < StringUtils::toLower(b.name);
    });
    return candidates;
  }

} // namespace

WallpaperProvider::WallpaperProvider(ConfigService* config, WaylandConnection* wayland)
    : m_config(config), m_wayland(wayland) {}

std::string WallpaperProvider::displayName() const { return i18n::tr("launcher.providers.wallpaper.title"); }

std::vector<LauncherResult> WallpaperProvider::query(std::string_view text) const {
  if (m_config == nullptr) {
    return {};
  }

  const std::string query = StringUtils::toLower(StringUtils::trim(text));
  auto candidates = collectWallpapers(
      wallpaper::resolveGlobalWallpaperDirectory(m_config->config().wallpaper, m_config->config().theme.mode)
  );
  if (candidates.empty()) {
    return {};
  }

  std::vector<std::pair<double, WallpaperCandidate>> scored;
  scored.reserve(candidates.size());
  for (auto& candidate : candidates) {
    const double score = query.empty() ? 0.0 : FuzzyMatch::score(query, candidate.searchable);
    if (query.empty() || FuzzyMatch::isMatch(score)) {
      scored.emplace_back(score, std::move(candidate));
    }
  }

  const auto limit = std::min(scored.size(), kMaxResults);
  std::partial_sort(
      scored.begin(), scored.begin() + static_cast<std::ptrdiff_t>(limit), scored.end(),
      [](const auto& a, const auto& b) { return a.first > b.first; }
  );

  std::vector<LauncherResult> results;
  results.reserve(limit);
  for (std::size_t i = 0; i < limit; ++i) {
    const auto& [score, candidate] = scored[i];
    LauncherResult result;
    result.id = candidate.path;
    result.title = candidate.name;
    result.subtitle = candidate.path;
    result.glyphName = "wallpaper-selector";
    result.iconPath = candidate.path;
    result.score = score;
    results.push_back(std::move(result));
  }
  return results;
}

bool WallpaperProvider::activate(const LauncherResult& result) {
  if (m_config == nullptr || result.id.empty()) {
    return false;
  }
  if (!result.providerId.empty() && result.providerId != id()) {
    return false;
  }

  const std::filesystem::path path(result.id);
  std::error_code ec;
  if (!hasImageExtension(path) || !std::filesystem::is_regular_file(path, ec) || ec) {
    return false;
  }

  ConfigService::WallpaperBatch batch(*m_config);
  if (m_wayland != nullptr) {
    for (const auto& out : m_wayland->outputs()) {
      if (!out.connectorName.empty()) {
        m_config->setWallpaperPath(out.connectorName, result.id);
      }
    }
  }
  m_config->setWallpaperPath(std::nullopt, result.id);
  return true;
}
