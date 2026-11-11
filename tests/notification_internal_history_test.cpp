#include "notification/notification_manager.h"
#include "tests/test_check.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace {

  const NotificationHistoryEntry* historyEntry(const NotificationManager& manager, std::uint32_t id) {
    const auto& history = manager.history();
    const auto it =
        std::ranges::find(history, id, [](const NotificationHistoryEntry& entry) { return entry.notification.id; });
    return it == history.end() ? nullptr : &*it;
  }

  bool historyContains(const NotificationManager& manager, std::string_view summary) {
    const auto& history = manager.history();
    return std::ranges::any_of(history, [summary](const NotificationHistoryEntry& entry) {
      return entry.notification.summary == summary;
    });
  }

} // namespace

int main() {
  NotificationManager manager;

  const auto addInternal = [&manager](std::string summary, bool persist, Urgency urgency = Urgency::Normal) {
    return manager.addOrReplace(
        NotificationRequest{
            .appName = "calendar-test",
            .summary = std::move(summary),
            .urgency = urgency,
            .origin = NotificationOrigin::Internal,
            .persistInHistory = persist,
        }
    );
  };

  // An internal notification stays toast-only by default, the pre-existing behavior.
  addInternal("transient-alert", false);
  TEST_CHECK(!historyContains(manager, "transient-alert"));

  // A one-shot internal alert (calendar reminder) opts in and survives being missed.
  addInternal("calendar-reminder", true);
  TEST_CHECK(historyContains(manager, "calendar-reminder"));

  // Low urgency is excluded from history unconditionally, opt-in or not.
  addInternal("low-urgency", true, Urgency::Low);
  TEST_CHECK(!historyContains(manager, "low-urgency"));

  // Calendar reminders emit timeout 0, which must mean "never expires" so they survive the user
  // stepping away from the desk.
  {
    const auto id = manager.addOrReplace(
        NotificationRequest{
            .appName = "calendar-test",
            .summary = "sticky-reminder",
            .urgency = Urgency::Normal,
            .timeout = 0,
            .origin = NotificationOrigin::Internal,
            .persistInHistory = true,
        }
    );
    bool foundPersistent = false;
    for (const Notification& n : manager.all()) {
      if (n.id == id) {
        foundPersistent = !n.expiryTime.has_value();
      }
    }
    TEST_CHECK(foundPersistent);
    TEST_CHECK(manager.expiredIds().empty());
  }

  // Actions on internal notifications are dispatched in-process, not over D-Bus: there is no client
  // to receive ActionInvoked, so signalling externally would go nowhere.
  {
    std::string externalKey;
    std::string internalKey;
    manager.setActionInvokeCallback(
        [&externalKey](uint32_t, const std::string& key, const std::string&, const std::string&) { externalKey = key; }
    );
    manager.setInternalActionCallback(
        [&internalKey](uint32_t, const std::string& key, const std::string&, const std::string&) { internalKey = key; }
    );

    const auto internalId = manager.addOrReplace(
        NotificationRequest{
            .appName = "calendar-test",
            .summary = "clickable-reminder",
            .origin = NotificationOrigin::Internal,
            .persistInHistory = true,
            .actions = {"default", "Open"},
        }
    );
    TEST_CHECK(manager.invokeAction(internalId, "default", false));
    TEST_CHECK(internalKey == "default");
    TEST_CHECK(externalKey.empty());

    // External notifications keep going to the D-Bus callback.
    const auto externalId = manager.addOrReplace(
        NotificationRequest{.appName = "other-app", .summary = "external-clickable", .actions = {"default", "Open"}}
    );
    TEST_CHECK(manager.invokeAction(externalId, "default", false));
    TEST_CHECK(externalKey == "default");

    manager.setActionInvokeCallback(nullptr);
    manager.setInternalActionCallback(nullptr);
  }

  // Dismissal is the only way a timeout-0 reminder can close, so it must not take the history entry
  // with it; otherwise opting into history buys nothing for exactly the notifications that need it.
  // Dismissal keeps history for every notification now (see notification_history_dismiss_test), so this
  // guards the retention path specifically for an entry that is only in history because of the flag.
  {
    const auto stickyId = manager.addOrReplace(
        NotificationRequest{
            .appName = "calendar-test",
            .summary = "dismissed-reminder",
            .timeout = 0,
            .origin = NotificationOrigin::Internal,
            .persistInHistory = true,
        }
    );
    TEST_CHECK(manager.close(stickyId, CloseReason::Dismissed));
    TEST_CHECK(historyContains(manager, "dismissed-reminder"));
    TEST_CHECK(!historyEntry(manager, stickyId)->active);
  }

  // A reminder counting down rewrites its own body. That must reach the history record without
  // reordering it, resetting its age, or marking an already-seen entry unread again.
  {
    const auto id = manager.addOrReplace(
        NotificationRequest{
            .appName = "calendar-test",
            .summary = "countdown",
            .body = "Starts in 10 min",
            .timeout = 0,
            .origin = NotificationOrigin::Internal,
            .persistInHistory = true,
        }
    );
    manager.addOrReplace(NotificationRequest{.appName = "other-app", .summary = "arrived-later"});
    const auto* before = historyEntry(manager, id);
    TEST_CHECK(before != nullptr);
    const auto receivedAt = before->notification.receivedTime;

    TEST_CHECK(manager.updateBody(id, "Starts now"));
    const auto* after = historyEntry(manager, id);
    TEST_CHECK(after != nullptr);
    TEST_CHECK(after->notification.body == "Starts now");
    TEST_CHECK(after->notification.receivedTime == receivedAt);
    TEST_CHECK(manager.history().back().notification.summary == "arrived-later");

    // A dismissed reminder must report as gone, or the caller would resurrect it as a new toast.
    TEST_CHECK(manager.close(id, CloseReason::Dismissed));
    TEST_CHECK(!manager.updateBody(id, "Starts now"));
    TEST_CHECK(!manager.updateBody(0xDEADBEEF, "nobody"));
  }

  // External notifications are unaffected.
  manager.addOrReplace(NotificationRequest{.appName = "other-app", .summary = "external"});
  TEST_CHECK(historyContains(manager, "external"));

  return 0;
}
