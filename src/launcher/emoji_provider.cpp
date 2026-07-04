#include "launcher/emoji_provider.h"

#include "core/files/resource_paths.h"
#include "i18n/i18n.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <filesystem>
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
} // namespace

void EmojiProvider::initialize() {
  const std::filesystem::path path = paths::assetPath("emoji.json");
  std::ifstream file(path);
  if (!file.is_open()) {
    return;
  }

  try {
    auto json = nlohmann::json::parse(file);
    if (!json.is_array()) {
      return;
    }

    m_entries.reserve(json.size());
    for (const auto& item : json) {
      EmojiEntry entry;
      entry.emoji = item.value("emoji", "");
      entry.name = item.value("name", "");
      entry.nameLower = StringUtils::toLower(entry.name);
      entry.category = item.value("category", "");

      if (item.contains("keywords") && item["keywords"].is_array()) {
        for (const auto& kw : item["keywords"]) {
          if (kw.is_string()) {
            entry.keywords.push_back(StringUtils::toLower(kw.get<std::string>()));
          }
        }
      }

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
  };
}

std::vector<LauncherResult> EmojiProvider::query(std::string_view text) const {
  std::string query = StringUtils::toLower(text);
  if (query.empty()) {
    std::vector<LauncherResult> results;
    results.reserve(m_entries.size());
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
      const auto& e = m_entries[i];
      LauncherResult r;
      r.id = "emoji-" + e.emoji;
      r.title = e.name;
      r.subtitle = emojiCategoryLabel(e.category);
      r.category = r.subtitle;
      r.badge = e.emoji;
      r.score = static_cast<int>(m_entries.size() - i);
      results.push_back(std::move(r));
    }
    return results;
  }

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

    if (bestScore > 0) {
      scored.push_back({bestScore, i});
    }
  }

  std::ranges::sort(scored, std::ranges::greater{}, &ScoredEntry::score);

  std::vector<LauncherResult> results;
  for (std::size_t i = 0; i < scored.size() && i < kMaxResults; ++i) {
    const auto& e = m_entries[scored[i].index];
    LauncherResult r;
    r.id = "emoji-" + e.emoji;
    r.title = e.name;
    r.subtitle = emojiCategoryLabel(e.category);
    r.category = r.subtitle;
    r.badge = e.emoji;
    r.score = scored[i].score;
    results.push_back(std::move(r));
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
