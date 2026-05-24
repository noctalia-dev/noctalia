#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>

struct NotificationHistoryEntry;

bool loadNotificationHistoryFromFile(
    const std::filesystem::path& path, std::deque<NotificationHistoryEntry>& out, std::uint32_t& outNextId,
    std::uint64_t& outChangeSerial
);

bool saveNotificationHistoryToFile(
    const std::filesystem::path& path, const std::deque<NotificationHistoryEntry>& entries, std::uint32_t nextId,
    std::uint64_t changeSerial
);
