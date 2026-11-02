#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>

namespace security {
  class SecureKey;
}

struct NotificationHistoryEntry;

inline constexpr std::size_t kNotificationHistoryMaxEntries = 100;

bool secureNotificationHistory(const std::filesystem::path& path);

bool loadNotificationHistoryFromFile(
    const std::filesystem::path& path, std::deque<NotificationHistoryEntry>& out, std::uint32_t& outNextId,
    std::uint64_t& outChangeSerial, const security::SecureKey& key
);

bool saveNotificationHistoryToFile(
    const std::filesystem::path& path, const std::deque<NotificationHistoryEntry>& entries, std::uint32_t nextId,
    std::uint64_t changeSerial, const security::SecureKey& key
);
