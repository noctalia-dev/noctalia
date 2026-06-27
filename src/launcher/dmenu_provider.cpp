#include "launcher/dmenu_provider.h"

#include "core/log.h"
#include "core/process.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace {

  constexpr std::chrono::milliseconds kCommandTimeout{2000};
  constexpr std::size_t kMaxOutputBytes = 256 * 1024;
  constexpr std::size_t kMaxResults = 200;

  // Replace every {selection} occurrence in `tmpl` with `selection`. Plain substitution;
  // the exec template is user-trusted config (like dmenu/rofi run commands).
  std::string substituteSelection(std::string tmpl, std::string_view selection) {
    constexpr std::string_view kToken = "{selection}";
    for (std::size_t pos = tmpl.find(kToken); pos != std::string::npos;
         pos = tmpl.find(kToken, pos + selection.size())) {
      tmpl.replace(pos, kToken.size(), selection);
    }
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
    logWarn("dmenu[{}]: command failed (exit {})", m_entry.id, result.exitCode);
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
  if (m_lines.empty()) {
    return {};
  }

  auto makeResult = [this](const Line& line, double score) {
    LauncherResult r;
    r.id = line.raw;
    r.title = line.title;
    r.subtitle = line.subtitle;
    r.glyphName = m_glyph;
    r.score = score;
    return r;
  };

  const std::string query = StringUtils::toLower(StringUtils::trim(text));
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
      const std::string command = substituteSelection(*m_entry.exec, line.raw);
      return process::runAsync(command);
    }
    return m_clipboard != nullptr && m_clipboard->copyText(line.raw);
  }
  return false;
}
