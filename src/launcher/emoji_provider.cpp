#include "launcher/emoji_provider.h"

#include "core/files/resource_paths.h"
#include "i18n/i18n.h"
#include "shell/wayland/clipboard_service.h"
#include "util/string_utils.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace {
  constexpr std::size_t kMaxResults = 100;

  // Resolve an emoji category id (as stored in emoji.json) to its localized label.
  std::string emojiCategoryLabel(std::string_view id) {
    return i18n::tr("launcher.categories.emoji." + std::string(id));
  }

  // Parse "u+2014" (1-6 hex digits) or a bare "2014" (4-6 hex digits) from a lowercased query.
  // Returns 0 when the query is not a codepoint.
  std::uint32_t parseCodepointQuery(std::string_view query) {
    const bool prefixed = query.starts_with("u+");
    if (prefixed) {
      query.remove_prefix(2);
    }
    const std::size_t minDigits = prefixed ? 1 : 4;
    if (query.size() < minDigits || query.size() > 6) {
      return 0;
    }
    std::uint32_t codepoint = 0;
    const auto [ptr, ec] = std::from_chars(query.data(), query.data() + query.size(), codepoint, 16);
    if (ec != std::errc{} || ptr != query.data() + query.size()) {
      return 0;
    }
    return codepoint;
  }
} // namespace

void EmojiProvider::ensureLoaded() const {
  if (m_loaded) {
    return;
  }
  // Set first so a missing or broken asset is not retried on every keystroke.
  m_loaded = true;
  // Emoji load first: on equal scores the stable sort in query() keeps them above Unicode entries.
  loadEntries("emoji.json", false);
  loadEntries("unicode.json", true);
}

void EmojiProvider::loadEntries(std::string_view assetName, bool unicode) const {
  const std::filesystem::path path = paths::assetPath(assetName);
  std::ifstream file(path);
  if (!file.is_open()) {
    return;
  }

  try {
    auto json = nlohmann::json::parse(file);
    if (!json.is_array()) {
      return;
    }

    m_entries.reserve(m_entries.size() + json.size());
    for (const auto& item : json) {
      EmojiEntry entry;
      if (unicode) {
        entry.emoji = item.value("char", "");
        entry.name = item.value("name", "");
        entry.codepoint = item.value("codepoint", std::uint32_t{0});
        entry.category = "unicode";
      } else {
        entry.emoji = item.value("emoji", "");
        entry.name = item.value("name", "");
        entry.category = item.value("category", "");

        if (item.contains("keywords") && item["keywords"].is_array()) {
          for (const auto& kw : item["keywords"]) {
            if (kw.is_string()) {
              entry.keywords.push_back(StringUtils::toLower(kw.get<std::string>()));
            }
          }
        }
      }
      entry.nameLower = StringUtils::toLower(entry.name);

      if (!entry.emoji.empty() && !entry.name.empty()) {
        m_entries.push_back(std::move(entry));
      }
    }
  } catch (...) {
    // Failed to parse JSON
  }
}

std::string EmojiProvider::displayName() const { return i18n::tr("launcher.providers.emoji.title"); }

std::vector<LauncherCategory> EmojiProvider::categories() const {
  return {
      {emojiCategoryLabel("people"), "mood-smile"},
      {emojiCategoryLabel("animals"), "paw"},
      {emojiCategoryLabel("food"), "apple"},
      {emojiCategoryLabel("travel"), "map"},
      {emojiCategoryLabel("activity"), "ball-football"},
      {emojiCategoryLabel("objects"), "device-floppy"},
      {emojiCategoryLabel("symbols"), "at"},
      {emojiCategoryLabel("flags"), "flag"},
      {emojiCategoryLabel("nature"), "leaf"},
      {emojiCategoryLabel("unicode"), "omega"},
  };
}

std::vector<LauncherResult> EmojiProvider::query(std::string_view text) const {
  ensureLoaded();

  const auto makeResult = [](const EmojiEntry& e, int score) {
    LauncherResult r;
    r.id = "emoji-" + e.emoji;
    r.title = e.name;
    r.category = emojiCategoryLabel(e.category);
    r.subtitle = e.codepoint != 0 ? std::format("U+{:04X}", e.codepoint) : r.category;
    r.badge = e.emoji;
    r.score = score;
    return r;
  };

  std::string query = StringUtils::toLower(text);
  if (query.empty()) {
    std::vector<LauncherResult> results;
    results.reserve(m_entries.size());
    for (const auto& e : m_entries) {
      results.push_back(makeResult(e, 0));
    }
    return results;
  }

  const std::uint32_t queryCodepoint = parseCodepointQuery(query);

  struct ScoredEntry {
    int score;
    std::size_t index;
  };

  std::vector<ScoredEntry> scored;

  for (std::size_t i = 0; i < m_entries.size(); ++i) {
    const auto& e = m_entries[i];
    int bestScore = 0;

    // Exact name match
    if (e.nameLower == query) {
      bestScore = 1000;
    }
    // Name prefix
    else if (e.nameLower.size() >= query.size() && e.nameLower.starts_with(query)) {
      bestScore = 500;
    }
    // Name contains
    else if (e.nameLower.contains(query)) {
      bestScore = 200;
    }
    // Keyword match
    else {
      for (const auto& kw : e.keywords) {
        if (kw == query) {
          bestScore = std::max(bestScore, 150);
        } else if (kw.size() >= query.size() && kw.starts_with(query)) {
          bestScore = std::max(bestScore, 100);
        } else if (kw.contains(query)) {
          bestScore = std::max(bestScore, 50);
        }
      }
    }

    if (queryCodepoint != 0 && e.codepoint == queryCodepoint) {
      bestScore = 1000;
    }

    if (bestScore > 0) {
      scored.push_back({bestScore, i});
    }
  }

  std::ranges::stable_sort(scored, std::ranges::greater{}, &ScoredEntry::score);

  std::vector<LauncherResult> results;
  for (std::size_t i = 0; i < scored.size() && i < kMaxResults; ++i) {
    results.push_back(makeResult(m_entries[scored[i].index], scored[i].score));
  }

  return results;
}

bool EmojiProvider::activate(const LauncherResult& result) {
  if (!result.id.starts_with("emoji-")) {
    return false;
  }

  std::string emoji = result.id.substr(6);
  return m_clipboard != nullptr && m_clipboard->copyText(std::move(emoji));
}
