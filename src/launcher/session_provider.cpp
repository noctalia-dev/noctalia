#include "launcher/session_provider.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "shell/session/session_action_meta.h"
#include "shell/session/session_action_runner.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

  constexpr std::size_t kMaxResults = 50;
  constexpr std::string_view kResultPrefix = "session:";

  struct SessionActionEntry {
    std::size_t index = 0;
    SessionPanelActionConfig config;
    std::string title;
    std::string glyph;
    std::string subtitle;
    std::string searchable;
  };

  [[nodiscard]] std::string aliasesForAction(std::string_view action) {
    if (action == "lock") {
      return "lock screen session";
    }
    if (action == "logout") {
      return "logout log out sign out exit session";
    }
    if (action == "suspend") {
      return "suspend sleep pause";
    }
    if (action == "lock_and_suspend") {
      return "lock and suspend sleep pause";
    }
    if (action == "reboot") {
      return "reboot restart";
    }
    if (action == "shutdown") {
      return "shutdown shut down power off poweroff";
    }
    return "custom command action";
  }

  [[nodiscard]] std::string resultId(std::size_t index, std::string_view action) {
    std::string id(kResultPrefix);
    id += std::to_string(index);
    id += ':';
    id += action;
    return id;
  }

  [[nodiscard]] bool parseResultId(std::string_view id, std::size_t& index, std::string_view& action) {
    if (!id.starts_with(kResultPrefix)) {
      return false;
    }
    id.remove_prefix(kResultPrefix.size());
    const auto separator = id.find(':');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= id.size()) {
      return false;
    }

    const std::string_view indexText = id.substr(0, separator);
    const auto [ptr, ec] = std::from_chars(indexText.data(), indexText.data() + indexText.size(), index);
    if (ec != std::errc{} || ptr != indexText.data() + indexText.size()) {
      return false;
    }

    action = id.substr(separator + 1);
    return session_action::isKnown(action);
  }

  [[nodiscard]] std::string actionSubtitle(const SessionPanelActionConfig& config) {
    if (config.command.has_value() && !StringUtils::trim(*config.command).empty()) {
      return i18n::tr("launcher.providers.session.command-subtitle");
    }
    return i18n::tr("launcher.providers.session.action-subtitle");
  }

  [[nodiscard]] std::vector<SessionActionEntry> collectActions(const ConfigService* config) {
    const std::vector<SessionPanelActionConfig> source =
        config != nullptr ? config->config().shell.session.actions : defaultSessionPanelActions();

    std::vector<SessionActionEntry> entries;
    entries.reserve(source.size());
    for (std::size_t i = 0; i < source.size(); ++i) {
      const SessionPanelActionConfig& row = source[i];
      if (!row.enabled || !session_action::isKnown(row.action)) {
        continue;
      }
      if (row.action == "command" && (!row.command.has_value() || StringUtils::trim(*row.command).empty())) {
        continue;
      }
      if ((row.action == "lock" || row.action == "lock_and_suspend")
          && config != nullptr
          && !config->isLockScreenEnabled()) {
        continue;
      }

      SessionActionEntry entry;
      entry.index = i;
      entry.config = row;
      entry.title =
          row.label.has_value() && !row.label->empty() ? *row.label : i18n::tr(session_action::labelKey(row.action));
      entry.glyph =
          row.glyph.has_value() && !row.glyph->empty() ? *row.glyph : session_action::defaultGlyph(row.action);
      entry.subtitle = actionSubtitle(row);
      entry.searchable = StringUtils::toLower(entry.title + " " + row.action + " " + aliasesForAction(row.action));
      if (row.command.has_value()) {
        entry.searchable += ' ';
        entry.searchable += StringUtils::toLower(*row.command);
      }
      entries.push_back(std::move(entry));
    }
    return entries;
  }

} // namespace

SessionProvider::SessionProvider(ConfigService* config, SessionActionRunner* actionRunner)
    : m_config(config), m_actionRunner(actionRunner) {}

std::string SessionProvider::displayName() const { return i18n::tr("launcher.providers.session.title"); }

std::vector<LauncherResult> SessionProvider::query(std::string_view text) const {
  auto entries = collectActions(m_config);
  if (entries.empty()) {
    return {};
  }

  auto makeResult = [](const SessionActionEntry& entry, double score) {
    LauncherResult result;
    result.id = resultId(entry.index, entry.config.action);
    result.title = entry.title;
    result.subtitle = entry.subtitle;
    result.glyphName = entry.glyph;
    result.score = score;
    return result;
  };

  const std::string query = StringUtils::toLower(StringUtils::trim(text));
  if (query.empty()) {
    const auto limit = std::min(entries.size(), kMaxResults);
    std::vector<LauncherResult> results;
    results.reserve(limit);
    for (std::size_t i = 0; i < limit; ++i) {
      results.push_back(makeResult(entries[i], 0.0));
    }
    return results;
  }

  std::vector<std::pair<double, SessionActionEntry>> scored;
  scored.reserve(entries.size());
  for (auto& entry : entries) {
    const double score = FuzzyMatch::score(query, entry.searchable);
    if (FuzzyMatch::isMatch(score)) {
      scored.emplace_back(score, std::move(entry));
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
    const auto& [score, entry] = scored[i];
    results.push_back(makeResult(entry, score));
  }
  return results;
}

bool SessionProvider::activate(const LauncherResult& result) {
  if (m_actionRunner == nullptr) {
    return false;
  }
  if (!result.providerId.empty() && result.providerId != id()) {
    return false;
  }

  std::size_t index = 0;
  std::string_view action;
  if (!parseResultId(result.id, index, action)) {
    return false;
  }

  for (const auto& entry : collectActions(m_config)) {
    if (entry.index == index && entry.config.action == action) {
      m_actionRunner->invoke(entry.config);
      return true;
    }
  }
  return false;
}
