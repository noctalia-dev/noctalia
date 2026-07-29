#include "launcher/dmenu_provider.h"

#include "core/log.h"
#include "core/process/process.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace {

  constexpr Logger kLog("dmenu");

  constexpr std::chrono::milliseconds kCommandTimeout{2000};
  constexpr std::size_t kMaxOutputBytes = 256 * 1024;
  constexpr std::size_t kMaxResults = 200;

  constexpr double kFreeformScore = 2048.0;

  void substituteToken(std::string& tmpl, std::string_view token, std::string_view value) {
    for (std::size_t pos = tmpl.find(token); pos != std::string::npos; pos = tmpl.find(token, pos + value.size())) {
      tmpl.replace(pos, token.size(), value);
    }
  }

  // Plain substitution; the exec template is user-trusted config (like dmenu/rofi run commands).
  std::string substituteExecTokens(std::string tmpl, std::string_view selection, std::string_view query) {
    substituteToken(tmpl, "{selection}", selection);
    substituteToken(tmpl, "{query}", query);
    return tmpl;
  }

} // namespace

DmenuProvider::Line DmenuProvider::parseLine(std::string&& raw) {
  Line line;
  if (const auto tab = raw.find('\t'); tab != std::string::npos) {
    line.title = raw.substr(0, tab);
    line.subtitle = raw.substr(tab + 1);
  } else {
    line.title = raw;
  }
  line.searchable = StringUtils::toLower(line.title + " " + line.subtitle);
  line.raw = std::move(raw);
  return line;
}

DmenuProvider::DmenuProvider(DmenuEntryConfig entry, ClipboardService* clipboard)
    : m_entry(std::move(entry)), m_clipboard(clipboard) {
  m_id = "dmenu.";
  m_id += m_entry.id;
  m_prefix = m_entry.prefix.value_or("");
  m_glyph = m_entry.glyph.value_or("terminal");
}

std::string DmenuProvider::displayName() const { return m_entry.label.value_or(m_entry.id); }

void DmenuProvider::ensureLoaded() const {
  if (m_loaded) {
    return;
  }
  m_loaded = true; // set before run so a failure doesn't retry every keystroke
  m_lines.clear();

  if (m_entry.command.empty()) {
    return;
  }
  const auto result =
      process::runSyncWithTimeoutAndOutputLimit({"/bin/sh", "-lc", m_entry.command}, kCommandTimeout, kMaxOutputBytes);
  if (!result) {
    kLog.warn("[{}] command failed (exit {})", m_entry.id, result.exitCode);
    return;
  }

  std::size_t begin = 0;
  for (std::size_t i = 0; i <= result.out.size(); ++i) {
    if (i < result.out.size() && result.out[i] != '\n') {
      continue;
    }
    std::size_t end = i;
    if (end > begin && result.out[end - 1] == '\r') {
      --end;
    }
    if (end > begin) {
      m_lines.push_back(parseLine(result.out.substr(begin, end - begin)));
    }
    begin = i + 1;
  }
}

void DmenuProvider::reset() {
  m_lines.clear();
  m_loaded = false;
}

std::vector<LauncherResult> DmenuProvider::query(std::string_view text) const {
  ensureLoaded();
  const std::string rawQuery = StringUtils::trim(text);
  if (m_lines.empty() && (!m_entry.freeform || rawQuery.empty())) {
    return {};
  }

  auto makeResult = [this, &rawQuery](const Line& line, double score) {
    LauncherResult r;
    r.id = line.raw;
    r.title = line.title;
    r.subtitle = line.subtitle;
    r.glyphName = m_glyph;
    r.query = rawQuery;
    r.score = score;
    return r;
  };
  auto makeFreeformResult = [this, &rawQuery] {
    LauncherResult r;
    r.id = rawQuery;
    r.title = rawQuery;
    r.glyphName = m_glyph;
    r.query = rawQuery;
    r.score = kFreeformScore;
    return r;
  };
  auto appendFreeformResult = [this, &rawQuery, &makeFreeformResult](std::vector<LauncherResult>& results) {
    if (!m_entry.freeform || rawQuery.empty()) {
      return;
    }
    const auto duplicate =
        std::ranges::any_of(results, [&rawQuery](const LauncherResult& result) { return result.id == rawQuery; });
    if (!duplicate) {
      results.push_back(makeFreeformResult());
    }
  };

  const std::string query = StringUtils::toLower(rawQuery);
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
    std::vector<LauncherResult> results;
    appendFreeformResult(results);
    return results;
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
  appendFreeformResult(results);
  return results;
}

bool DmenuProvider::activate(const LauncherResult& result) {
  if (!result.providerId.empty() && result.providerId != m_id) {
    return false;
  }
  // Only activate lines this provider actually produced.
  for (const auto& line : m_lines) {
    if (line.raw != result.id) {
      continue;
    }
    if (m_entry.exec.has_value() && !m_entry.exec->empty()) {
      const std::string command = substituteExecTokens(*m_entry.exec, line.raw, result.query.value_or(""));
      return process::runAsync(command);
    }
    return m_clipboard != nullptr && m_clipboard->copyText(line.raw);
  }
  if (m_entry.freeform && result.query.has_value() && result.id == *result.query && !result.id.empty()) {
    if (m_entry.exec.has_value() && !m_entry.exec->empty()) {
      const std::string command = substituteExecTokens(*m_entry.exec, result.id, *result.query);
      return process::runAsync(command);
    }
    return m_clipboard != nullptr && m_clipboard->copyText(result.id);
  }
  return false;
}
