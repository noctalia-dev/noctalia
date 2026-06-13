#include "launcher/app_provider.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "i18n/i18n.h"
#include "system/desktop_entry_launch.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace {

  constexpr std::size_t kMaxSearchResults = 50;
  constexpr std::string_view kDefaultAppIcon = "application-x-executable";

  double scoreEntry(std::string_view pattern, const DesktopEntry& entry) {
    if (pattern.empty()) {
      return 0.0;
    }

    double nameScore = FuzzyMatch::score(pattern, entry.nameLower) * 5.0;
    if (FuzzyMatch::isMatch(nameScore) && entry.nameLower.starts_with(pattern)) {
      nameScore += 500.0;
    }
    const double genericScore = FuzzyMatch::score(pattern, entry.genericNameLower) * 2.0;

    auto scoreList = [&](std::string_view list, double weight) {
      double best = FuzzyMatch::noMatchScore;
      std::size_t start = 0;
      while (start < list.size()) {
        auto semi = list.find(';', start);
        auto word = (semi == std::string_view::npos) ? list.substr(start) : list.substr(start, semi - start);
        if (!word.empty()) {
          best = std::max(best, FuzzyMatch::score(pattern, word) * weight);
        }
        if (semi == std::string_view::npos)
          break;
        start = semi + 1;
      }
      return best;
    };

    const double keywordScore = scoreList(entry.keywordsLower, 0.8);
    const double catScore = scoreList(entry.categoriesLower, 0.3);
    const double idScore = FuzzyMatch::score(pattern, entry.idLower) * 1.5;
    const double execScore = FuzzyMatch::score(pattern, entry.execLower);

    return std::max({nameScore, genericScore, keywordScore, catScore, idScore, execScore});
  }

  struct AppCategoryDef {
    std::string_view id;
    std::string_view glyph;
  };

  // Stable category ids (used for matching) paired with their chip glyph. Display
  // labels are resolved from i18n via appCategoryLabel(), keyed on the id.
  constexpr std::array<AppCategoryDef, 9> kAppCategories = {{
      {"internet", "world"},
      {"multimedia", "player-play"},
      {"development", "code"},
      {"games", "device-gamepad-2"},
      {"graphics", "photo"},
      {"office", "briefcase"},
      {"education", "school"},
      {"system", "settings"},
      {"utilities", "tool"},
  }};

  std::string appCategoryLabel(std::string_view id) {
    return i18n::tr("launcher.categories.applications." + std::string(id));
  }

  // Maps a desktop-entry category list to one of kAppCategories' stable ids.
  std::string_view primaryCategory(std::string_view categories) {
    std::size_t start = 0;
    while (start < categories.size()) {
      auto semi = categories.find(';', start);
      auto token = (semi == std::string_view::npos) ? categories.substr(start) : categories.substr(start, semi - start);
      if (token == "AudioVideo" || token == "Audio" || token == "Video") {
        return "multimedia";
      }
      if (token == "Development") {
        return "development";
      }
      if (token == "Game") {
        return "games";
      }
      if (token == "Graphics") {
        return "graphics";
      }
      if (token == "Network") {
        return "internet";
      }
      if (token == "Office") {
        return "office";
      }
      if (token == "System") {
        return "system";
      }
      if (token == "Utility" || token == "Settings") {
        return "utilities";
      }
      if (token == "Education" || token == "Science") {
        return "education";
      }
      if (semi == std::string_view::npos) {
        break;
      }
      start = semi + 1;
    }
    return {};
  }

} // namespace

AppProvider::AppProvider(ConfigService* config, CompositorPlatform* platform)
    : m_config(config), m_platform(platform) {}

void AppProvider::initialize() { refreshEntriesIfNeeded(); }

std::string AppProvider::displayName() const { return i18n::tr("launcher.providers.applications.title"); }

std::vector<LauncherCategory> AppProvider::categories() const {
  refreshEntriesIfNeeded();

  std::array<bool, kAppCategories.size()> populated{};
  for (const auto& entry : m_entries) {
    const std::string_view categoryId = primaryCategory(entry.categories);
    if (categoryId.empty()) {
      continue;
    }
    for (std::size_t i = 0; i < kAppCategories.size(); ++i) {
      if (kAppCategories[i].id == categoryId) {
        populated[i] = true;
        break;
      }
    }
  }

  std::vector<LauncherCategory> result;
  for (std::size_t i = 0; i < kAppCategories.size(); ++i) {
    if (!populated[i]) {
      continue;
    }
    result.push_back({appCategoryLabel(kAppCategories[i].id), std::string(kAppCategories[i].glyph)});
  }
  return result;
}

void AppProvider::refreshEntriesIfNeeded() const {
  const auto version = desktopEntriesVersion();
  if (version == m_entriesVersion) {
    return;
  }

  m_entries = desktopEntries();
  m_entriesVersion = version;
}

std::vector<LauncherResult> AppProvider::query(std::string_view text) const {
  refreshEntriesIfNeeded();
  const std::string normalizedText = StringUtils::toLower(text);
  const std::string_view pattern = normalizedText;

  auto buildResult = [&](const DesktopEntry& entry, double s) {
    LauncherResult result;
    result.id = entry.path;
    result.title = entry.name;
    result.subtitle = entry.genericName.empty() ? entry.comment : entry.genericName;
    result.iconName = entry.icon.empty() ? std::string(kDefaultAppIcon) : entry.icon;
    result.glyphName = "app-window";
    const std::string_view categoryId = primaryCategory(entry.categories);
    result.category = categoryId.empty() ? std::string() : appCategoryLabel(categoryId);
    result.score = s;
    return result;
  };

  // Empty query: return all entries in alphabetical order (as stored)
  if (pattern.empty()) {
    std::vector<LauncherResult> results;
    results.reserve(m_entries.size());
    for (const auto& entry : m_entries) {
      results.push_back(buildResult(entry, 0));
    }
    return results;
  }

  std::vector<std::pair<double, const DesktopEntry*>> scored;
  for (const auto& entry : m_entries) {
    const double s = scoreEntry(pattern, entry);
    if (FuzzyMatch::isMatch(s)) {
      scored.emplace_back(s, &entry);
    }
  }
  const auto cmp = [](const auto& a, const auto& b) { return a.first > b.first; };
  const std::size_t limit = std::min(scored.size(), kMaxSearchResults);
  std::partial_sort(scored.begin(), scored.begin() + static_cast<std::ptrdiff_t>(limit), scored.end(), cmp);

  std::vector<LauncherResult> results;
  results.reserve(limit);
  for (std::size_t i = 0; i < limit; ++i) {
    const auto& [s, entry] = scored[i];
    results.push_back(buildResult(*entry, s));
  }
  return results;
}

bool AppProvider::activate(const LauncherResult& result) {
  refreshEntriesIfNeeded();

  for (const auto& entry : m_entries) {
    if (entry.path != result.id) {
      continue;
    }

    const DesktopAction* chosen = nullptr;
    if (!result.desktopActionId.empty()) {
      for (const auto& action : entry.actions) {
        if (action.id == result.desktopActionId) {
          chosen = &action;
          break;
        }
      }
      if (chosen == nullptr || chosen->exec.empty()) {
        return false;
      }
    }

    std::string token;
    if (m_platform != nullptr && m_platform->hasXdgActivation()) {
      token = m_platform->requestActivationToken(nullptr);
    }
    desktop_entry_launch::LaunchOptions launchOptions{
        .activationToken = std::move(token),
        .runAsSystemdService = m_config->config().shell.launchAppsAsSystemdServices,
        .customCommand = m_config->config().shell.launchAppsCustomCommand,
    };

    if (chosen != nullptr) {
      return desktop_entry_launch::launchAction(*chosen, entry.id, entry.workingDir, entry.terminal, launchOptions);
    }
    return desktop_entry_launch::launchEntry(entry, launchOptions);
  }
  return false;
}
