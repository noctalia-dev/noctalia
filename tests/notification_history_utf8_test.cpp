#include "notification/notification_history_store.h"
#include "notification/notification_manager.h"
#include "notification_history_test_utils.h"
#include "tests/test_check.h"
#include "util/string_utils.h"

#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>

namespace {

  Notification makeNotification(std::string body) {
    const auto now = Clock::now();
    const auto wallNow = WallClock::now();
    return Notification{
        .id = 314,
        .origin = NotificationOrigin::External,
        .appName = "test",
        .summary = "summary",
        .body = std::move(body),
        .timeout = 6000,
        .urgency = Urgency::Normal,
        .actions = {},
        .icon = std::nullopt,
        .imageData = std::nullopt,
        .category = std::nullopt,
        .desktopEntry = std::nullopt,
        .receivedTime = now,
        .expiryTime = std::nullopt,
        .receivedWallClock = wallNow,
        .expiryWallClock = std::nullopt,
    };
  }

  struct TempHistory {
    std::filesystem::path dir;
    std::filesystem::path path;

    explicit TempHistory(const std::string& suffix)
        : dir(std::filesystem::temp_directory_path()
              / ("noctalia-test-" + std::to_string(static_cast<long long>(getpid())) + "-" + suffix)),
          path(dir / "history.enc") {
      std::filesystem::remove_all(dir);
      std::filesystem::create_directories(dir);
    }

    ~TempHistory() { std::filesystem::remove_all(dir); }
  };

} // namespace

int main() {
  // 2-byte: split at boundary
  {
    std::string s(1023, 'a');
    s.push_back(static_cast<char>(0xD1));
    s.push_back(static_cast<char>(0x80));
    TEST_CHECK(StringUtils::truncateUtf8(s, 1024).size() == 1023);
  }

  // 2-byte: fits at boundary
  {
    std::string s(1022, 'a');
    s.push_back(static_cast<char>(0xD1));
    s.push_back(static_cast<char>(0x80));
    TEST_CHECK(StringUtils::truncateUtf8(s, 1024).size() == 1024);
  }

  // 4-byte: split after 1st byte
  {
    std::string s(1023, 'a');
    s.push_back(static_cast<char>(0xF0));
    s.push_back(static_cast<char>(0x9F));
    s.push_back(static_cast<char>(0x98));
    s.push_back(static_cast<char>(0x80));
    TEST_CHECK(StringUtils::truncateUtf8(s, 1024).size() == 1023);
  }

  // 4-byte: split after 2nd byte
  {
    std::string s(1022, 'a');
    s.push_back(static_cast<char>(0xF0));
    s.push_back(static_cast<char>(0x9F));
    s.push_back(static_cast<char>(0x98));
    s.push_back(static_cast<char>(0x80));
    TEST_CHECK(StringUtils::truncateUtf8(s, 1024).size() == 1022);
  }

  // 4-byte: split after 3rd byte
  {
    std::string s(1021, 'a');
    s.push_back(static_cast<char>(0xF0));
    s.push_back(static_cast<char>(0x9F));
    s.push_back(static_cast<char>(0x98));
    s.push_back(static_cast<char>(0x80));
    TEST_CHECK(StringUtils::truncateUtf8(s, 1024).size() == 1021);
  }

  // 4-byte: fits exactly
  {
    std::string s(1020, 'a');
    s.push_back(static_cast<char>(0xF0));
    s.push_back(static_cast<char>(0x9F));
    s.push_back(static_cast<char>(0x98));
    s.push_back(static_cast<char>(0x80));
    TEST_CHECK(StringUtils::truncateUtf8(s, 1024).size() == 1024);
  }

  // Persistence: malformed body serializes without throwing and produces valid JSON
  {
    TempHistory tmp("utf8-history");
    const auto key = notificationHistoryTestKey();

    std::string body = "prefix ";
    body.push_back(static_cast<char>(0xD1));

    std::deque<NotificationHistoryEntry> entries;
    entries.push_back(
        NotificationHistoryEntry{
            .notification = makeNotification(std::move(body)),
            .active = true,
            .closeReason = std::nullopt,
            .eventSerial = 1,
        }
    );

    bool saved = false;
    try {
      saved = saveNotificationHistoryToFile(tmp.path, entries, 315, 2, key);
    } catch (const std::exception& e) {
      std::println(stderr, "saveNotificationHistoryToFile threw: {}", e.what());
    }

    const auto entryCount = readNotificationHistoryJson(tmp.path, key).at("entries").size();
    // Remove the temp dir before checking: TEST_CHECK exits without running destructors.
    std::filesystem::remove_all(tmp.dir);
    TEST_CHECK(saved);
    TEST_CHECK(entryCount == 1);
  }

  return 0;
}
