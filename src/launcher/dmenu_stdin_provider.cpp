#include "launcher/dmenu_stdin_provider.h"

#include "util/fuzzy_match.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace {

  constexpr std::size_t kMaxResults = 500;

} // namespace

DmenuStdinProvider::DmenuStdinProvider(std::vector<std::string> lines, std::string id, Completion completion)
    : m_id(std::move(id)), m_completion(std::move(completion)) {
  m_lines.reserve(lines.size());
  for (auto& raw : lines) {
    if (raw.empty()) {
      continue;
    }
    Line line;
    if (const auto tab = raw.find('\t'); tab != std::string::npos) {
      line.title = raw.substr(0, tab);
      line.subtitle = raw.substr(tab + 1);
    } else {
      line.title = raw;
    }
    line.searchable = StringUtils::toLower(line.title + " " + line.subtitle);
    line.raw = std::move(raw);
    m_lines.push_back(std::move(line));
  }
}

std::vector<LauncherResult> DmenuStdinProvider::query(std::string_view text) const {
  auto makeResult = [](const Line& line, double score) {
    LauncherResult r;
    r.id = line.raw;
    r.title = line.title;
    r.subtitle = line.subtitle;
    r.glyphName = "terminal";
    r.score = score;
    return r;
  };
  auto makeFreeformResult = [](const std::string& rawQuery) {
    LauncherResult result;
    result.id = rawQuery;
    result.title = rawQuery;
    result.glyphName = "terminal";
    result.query = rawQuery;
    return result;
  };

  const std::string rawQuery(text);
  const std::string query = StringUtils::toLower(StringUtils::trim(text));
  if (m_lines.empty()) {
    return query.empty() ? std::vector<LauncherResult>{} : std::vector<LauncherResult>{makeFreeformResult(rawQuery)};
  }
  if (query.empty()) {
    const auto limit = std::min(m_lines.size(), kMaxResults);
    std::vector<LauncherResult> results;
    results.reserve(limit);
    for (std::size_t i = 0; i < limit; ++i) {
      results.push_back(makeResult(m_lines[i], 0.0));
    }
    return results;
  }

  std::vector<std::pair<double, const Line*>> scored;
  scored.reserve(m_lines.size());
  for (const auto& line : m_lines) {
    const double s = FuzzyMatch::score(query, line.searchable);
    if (FuzzyMatch::isMatch(s)) {
      scored.emplace_back(s, &line);
    }
  }
  if (scored.empty()) {
    return {makeFreeformResult(rawQuery)};
  }

  const auto limit = std::min(scored.size(), kMaxResults);
  std::partial_sort(
      scored.begin(), scored.begin() + static_cast<std::ptrdiff_t>(limit), scored.end(),
      [](const auto& a, const auto& b) { return a.first > b.first; }
  );

  std::vector<LauncherResult> results;
  results.reserve(limit);
  for (std::size_t i = 0; i < limit; ++i) {
    results.push_back(makeResult(*scored[i].second, scored[i].first));
  }
  return results;
}

bool DmenuStdinProvider::activate(const LauncherResult& result) {
  if (m_completed) {
    return false;
  }
  if (!result.providerId.empty() && result.providerId != m_id) {
    return false;
  }
  for (const auto& line : m_lines) {
    if (line.raw != result.id) {
      continue;
    }
    m_completed = true;
    if (m_completion) {
      m_completion(line.raw);
    }
    return true;
  }
  if (result.query.has_value() && result.id == *result.query && !result.id.empty()) {
    m_completed = true;
    if (m_completion) {
      m_completion(result.id);
    }
    return true;
  }
  return false;
}

void DmenuStdinProvider::reset() {
  if (m_completed) {
    return;
  }
  m_completed = true;
  if (m_completion) {
    m_completion(std::nullopt);
  }
}
