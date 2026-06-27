#include "launcher/window_provider.h"

#include "compositors/compositor_platform.h"
#include "i18n/i18n.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

  constexpr std::size_t kMaxResults = 50;

  struct WindowCandidate {
    WorkspaceWindowAssignment window;
    std::string title;
    std::string searchable;
  };

  [[nodiscard]] std::vector<WindowCandidate> collectWindows(const CompositorPlatform* platform) {
    std::vector<WindowCandidate> candidates;
    if (platform == nullptr) {
      return candidates;
    }

    // No output filter: surface windows from every monitor/workspace.
    auto assignments = platform->workspaceWindowAssignments();
    candidates.reserve(assignments.size());
    for (auto& assignment : assignments) {
      if (assignment.windowId.empty()) {
        continue;
      }
      WindowCandidate candidate;
      candidate.title = !assignment.title.empty() ? assignment.title : assignment.appId;
      candidate.searchable = StringUtils::toLower(candidate.title + " " + assignment.appId);
      candidate.window = std::move(assignment);
      candidates.push_back(std::move(candidate));
    }

    std::ranges::sort(candidates, [](const WindowCandidate& a, const WindowCandidate& b) {
      return StringUtils::toLower(a.title) < StringUtils::toLower(b.title);
    });
    return candidates;
  }

} // namespace

WindowProvider::WindowProvider(CompositorPlatform* platform) : m_platform(platform) {}

std::string WindowProvider::displayName() const { return i18n::tr("launcher.providers.window.title"); }

std::vector<LauncherResult> WindowProvider::query(std::string_view text) const {
  auto candidates = collectWindows(m_platform);
  if (candidates.empty()) {
    return {};
  }

  auto makeResult = [](const WindowCandidate& candidate, double score) {
    LauncherResult result;
    result.id = candidate.window.windowId;
    result.title = candidate.title;
    result.subtitle = candidate.window.appId;
    result.iconName = candidate.window.appId;
    result.glyphName = "app-window";
    result.score = score;
    return result;
  };

  const std::string query = StringUtils::toLower(StringUtils::trim(text));
  if (query.empty()) {
    const auto limit = std::min(candidates.size(), kMaxResults);
    std::vector<LauncherResult> results;
    results.reserve(limit);
    for (std::size_t i = 0; i < limit; ++i) {
      results.push_back(makeResult(candidates[i], 0.0));
    }
    return results;
  }

  std::vector<std::pair<double, WindowCandidate>> scored;
  scored.reserve(candidates.size());
  for (auto& candidate : candidates) {
    const double score = FuzzyMatch::score(query, candidate.searchable);
    if (FuzzyMatch::isMatch(score)) {
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
    results.push_back(makeResult(candidate, score));
  }
  return results;
}

bool WindowProvider::activate(const LauncherResult& result) {
  if (m_platform == nullptr || result.id.empty()) {
    return false;
  }
  if (!result.providerId.empty() && result.providerId != id()) {
    return false;
  }

  m_platform->focusCompositorWindow(result.id);
  return true;
}
