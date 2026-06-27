#pragma once

#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>

// Tracks how many times each launcher result has been activated.
// Providers that opt in via LauncherProvider::trackUsage() get their results
// score-boosted based on activation history, surfacing frequently used entries.
class UsageTracker {
public:
  UsageTracker();

  void record(std::string_view providerId, std::string_view resultId);
  void clear();
  [[nodiscard]] int getCount(std::string_view providerId, std::string_view resultId) const;
  [[nodiscard]] int getRecentlyUsedIndex(std::string_view providerId, std::string_view resultId) const;
  [[nodiscard]] std::size_t getRecentlyUsedCount(std::string_view providerId) const;

private:
  void load();
  void save() const;

  std::string m_usageCountsPath;
  std::string m_recentlyUsedPath;
  std::unordered_map<std::string, std::unordered_map<std::string, int>> m_counts;
  std::unordered_map<std::string, std::deque<std::string>> m_recentlyUsed;
  std::unordered_map<std::string, std::unordered_map<std::string, int>> m_recentlyUsedIndex;
};
