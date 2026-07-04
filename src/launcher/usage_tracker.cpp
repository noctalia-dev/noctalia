#include "launcher/usage_tracker.h"

#include "util/file_utils.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {
  constexpr std::size_t kMaxRecentlyUsedCount = 20;
}

UsageTracker::UsageTracker() {
  const std::string dir = FileUtils::stateDir();
  m_usageCountsPath = (dir.empty() ? "." : dir) + "/usage_counts.json";
  m_recentlyUsedPath = (dir.empty() ? "." : dir) + "/recently_used.json";
  load();
}

void UsageTracker::record(std::string_view providerId, std::string_view resultId) {
  ++m_counts[std::string(providerId)][std::string(resultId)];

  auto& recentlyUsedList = m_recentlyUsed[std::string(providerId)];
  const auto id = std::string(resultId);
  std::erase(recentlyUsedList, id);
  recentlyUsedList.push_front(id);
  while (recentlyUsedList.size() > kMaxRecentlyUsedCount) {
    recentlyUsedList.pop_back();
  }
  auto& indexMap = m_recentlyUsedIndex[std::string(providerId)];
  indexMap.clear();
  for (std::size_t i = 0; i < recentlyUsedList.size(); ++i) {
    indexMap[recentlyUsedList[i]] = static_cast<int>(recentlyUsedList.size() - i);
  }

  save();
}

void UsageTracker::clear() {
  m_counts.clear();
  m_recentlyUsed.clear();
  m_recentlyUsedIndex.clear();
  save();
}

int UsageTracker::getCount(std::string_view providerId, std::string_view resultId) const {
  const auto provIt = m_counts.find(std::string(providerId));
  if (provIt == m_counts.end()) {
    return 0;
  }
  const auto idIt = provIt->second.find(std::string(resultId));
  return idIt != provIt->second.end() ? idIt->second : 0;
}

int UsageTracker::getRecentlyUsedIndex(std::string_view providerId, std::string_view resultId) const {
  const auto provIt = m_recentlyUsedIndex.find(std::string(providerId));
  if (provIt == m_recentlyUsedIndex.end()) {
    return 0;
  }
  const auto idIt = provIt->second.find(std::string(resultId));
  return idIt != provIt->second.end() ? idIt->second : 0;
}

std::size_t UsageTracker::getRecentlyUsedCount(std::string_view providerId) const {
  const auto provIt = m_recentlyUsed.find(std::string(providerId));
  return provIt != m_recentlyUsed.end() ? provIt->second.size() : 0;
}

void UsageTracker::load() {
  {
    std::ifstream file(m_usageCountsPath);
    if (file.is_open()) {
      try {
        const auto json = nlohmann::json::parse(file);
        for (const auto& [provider, ids] : json.items()) {
          for (const auto& [id, count] : ids.items()) {
            m_counts[provider][id] = count.get<int>();
          }
        }
      } catch (const nlohmann::json::exception&) {
        // Ignore malformed file — starts fresh
      }
    }
  }
  {
    std::ifstream file(m_recentlyUsedPath);
    if (file.is_open()) {
      try {
        const auto json = nlohmann::json::parse(file);
        for (const auto& [provider, ids] : json.items()) {
          for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto& id = ids[i].get<std::string>();
            m_recentlyUsed[provider].push_back(id);
            m_recentlyUsedIndex[provider][id] = static_cast<int>(ids.size() - i);
          }
        }
      } catch (const nlohmann::json::exception&) {
        // Ignore malformed file — starts fresh
      }
    }
  }
}

void UsageTracker::save() const {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(m_usageCountsPath).parent_path(), ec);
  if (ec) {
    return;
  }
  {
    nlohmann::json json = m_counts;
    std::ofstream file(m_usageCountsPath, std::ios::trunc);
    file << json.dump(2) << '\n';
  }
  {
    nlohmann::json json = m_recentlyUsed;
    std::ofstream file(m_recentlyUsedPath, std::ios::trunc);
    file << json.dump(2) << '\n';
  }
}
