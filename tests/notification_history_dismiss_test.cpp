#include "notification/notification_manager.h"
#include "tests/test_check.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

  uint32_t addExternal(NotificationManager& manager, std::string summary) {
    return manager.addOrReplace(
        NotificationRequest{
            .appName = "history-test",
            .summary = std::move(summary),
            .timeout = 0,
        }
    );
  }

  const NotificationHistoryEntry* findEntry(const NotificationManager& manager, uint32_t id) {
    for (const auto& entry : manager.history()) {
      if (entry.notification.id == id) {
        return &entry;
      }
    }
    return nullptr;
  }

} // namespace

int main() {
  NotificationManager manager;
  std::vector<std::pair<uint32_t, CloseReason>> closes;
  manager.setCloseCallback([&closes](uint32_t id, CloseReason reason) { closes.emplace_back(id, reason); });

  const uint32_t firstId = addExternal(manager, "first");
  const uint32_t secondId = addExternal(manager, "second");
  TEST_CHECK(manager.all().size() == 2);
  TEST_CHECK(manager.history().size() == 2);

  // Dismissing hides the toast but keeps the history entry, in place and unread.
  TEST_CHECK(manager.close(firstId, CloseReason::Dismissed));
  TEST_CHECK(manager.all().size() == 1);
  TEST_CHECK(manager.all().front().id == secondId);
  TEST_CHECK(manager.history().size() == 2);
  TEST_CHECK(manager.history().front().notification.id == firstId);

  const NotificationHistoryEntry* dismissed = findEntry(manager, firstId);
  TEST_CHECK(dismissed != nullptr);
  TEST_CHECK(!dismissed->active);
  TEST_CHECK(dismissed->closeReason == CloseReason::Dismissed);
  TEST_CHECK(!dismissed->seen);
  TEST_CHECK(manager.hasUnreadNotificationHistory());

  // The owner app still learns about the dismissal immediately.
  TEST_CHECK(closes.size() == 1);
  TEST_CHECK(closes.front().first == firstId);
  TEST_CHECK(closes.front().second == CloseReason::Dismissed);

  // Expiring behaves the same way, and also keeps the arrival order.
  TEST_CHECK(manager.close(secondId, CloseReason::Expired));
  TEST_CHECK(manager.all().empty());
  TEST_CHECK(manager.history().size() == 2);
  TEST_CHECK(manager.history().back().notification.id == secondId);
  TEST_CHECK(manager.history().back().closeReason == CloseReason::Expired);

  manager.markNotificationHistorySeen();
  TEST_CHECK(!manager.hasUnreadNotificationHistory());

  // Only explicit history removal deletes entries.
  manager.removeHistoryEntry(firstId);
  TEST_CHECK(manager.history().size() == 1);
  TEST_CHECK(findEntry(manager, firstId) == nullptr);
  manager.clearHistory();
  TEST_CHECK(manager.history().empty());

  // Filtered-out senders stay out of history when dismissed.
  manager.setFilters({NotificationFilterConfig{
      .name = "no-history",
      .match = "history-test",
      .saveHistory = false,
  }});
  const uint32_t filteredId = addExternal(manager, "filtered");
  TEST_CHECK(manager.history().empty());
  TEST_CHECK(manager.close(filteredId, CloseReason::Dismissed));
  TEST_CHECK(manager.history().empty());

  return 0;
}
