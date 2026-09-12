#include "notification/notification_manager.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

namespace {

  bool check(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
  }

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
  bool ok = true;
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

  // An internal notification stays toast-only by default — the pre-existing behavior.
  (void)addInternal("transient-alert", false);
  ok = check(!historyContains(manager, "transient-alert"), "an internal notification was persisted without opting in")
      && ok;

  // A one-shot internal alert (calendar reminder) opts in and survives being missed.
  (void)addInternal("calendar-reminder", true);
  ok =
      check(historyContains(manager, "calendar-reminder"), "an opted-in internal notification was not persisted") && ok;

  // Low urgency is excluded from history unconditionally, opt-in or not.
  (void)addInternal("low-urgency", true, Urgency::Low);
  ok = check(!historyContains(manager, "low-urgency"), "a low urgency notification was persisted") && ok;

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
    ok = check(foundPersistent, "a zero timeout reminder was given an expiry and would auto-dismiss") && ok;
    ok = check(manager.expiredIds().empty(), "a persistent reminder was reported as expired") && ok;
  }

  // Actions on internal notifications are dispatched in-process, not over D-Bus: there is no client
  // to receive ActionInvoked, so signalling externally would go nowhere.
  {
    std::string externalKey;
    std::string internalKey;
    manager.setActionInvokeCallback([&externalKey](uint32_t, const std::string& key, const std::string&) {
      externalKey = key;
    });
    manager.setInternalActionCallback([&internalKey](uint32_t, const std::string& key, const std::string&) {
      internalKey = key;
    });

    const auto internalId = manager.addOrReplace(
        NotificationRequest{
            .appName = "calendar-test",
            .summary = "clickable-reminder",
            .origin = NotificationOrigin::Internal,
            .persistInHistory = true,
            .actions = {"default", "Open"},
        }
    );
    ok = check(
             manager.invokeAction(internalId, "default", false),
             "a default action on an internal notification "
             "could not be invoked"
         )
        && ok;
    ok = check(internalKey == "default", "an internal notification action was not dispatched in-process") && ok;
    ok = check(externalKey.empty(), "an internal notification action leaked to the D-Bus callback") && ok;

    // External notifications keep going to the D-Bus callback.
    const auto externalId = manager.addOrReplace(
        NotificationRequest{.appName = "other-app", .summary = "external-clickable", .actions = {"default", "Open"}}
    );
    ok = check(manager.invokeAction(externalId, "default", false), "an external default action could not be invoked")
        && ok;
    ok = check(externalKey == "default", "an external notification action stopped reaching the D-Bus callback") && ok;

    manager.setActionInvokeCallback(nullptr);
    manager.setInternalActionCallback(nullptr);
  }

  // Dismissal is the only way a timeout-0 reminder can close, so it must not take the history entry
  // with it — otherwise opting into history buys nothing for exactly the notifications that need it.
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
    ok = check(manager.close(stickyId, CloseReason::Dismissed), "a persistent reminder could not be dismissed") && ok;
    ok = check(historyContains(manager, "dismissed-reminder"), "dismissing a persistent reminder erased its history")
        && ok;
    ok = check(!historyEntry(manager, stickyId)->active, "a dismissed reminder was still marked active in history")
        && ok;
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
    (void)manager.addOrReplace(NotificationRequest{.appName = "other-app", .summary = "arrived-later"});
    const auto* before = historyEntry(manager, id);
    ok = check(before != nullptr, "a persistent reminder never reached history") && ok;
    const auto receivedAt = before != nullptr ? before->notification.receivedTime : TimePoint{};

    ok = check(manager.updateBody(id, "Starts now"), "updateBody rejected a live notification") && ok;
    const auto* after = historyEntry(manager, id);
    ok = check(after != nullptr && after->notification.body == "Starts now", "updateBody did not reach history") && ok;
    ok = check(after != nullptr && after->notification.receivedTime == receivedAt, "updateBody aged the notification")
        && ok;
    ok = check(
             manager.history().back().notification.summary == "arrived-later",
             "updateBody shoved the reminder back to the top of the notification list"
         )
        && ok;

    // A dismissed reminder must report as gone, or the caller would resurrect it as a new toast.
    ok = check(manager.close(id, CloseReason::Dismissed), "the countdown reminder could not be dismissed") && ok;
    ok = check(!manager.updateBody(id, "Starts now"), "updateBody accepted a dismissed notification") && ok;
    ok = check(!manager.updateBody(0xDEADBEEF, "nobody"), "updateBody accepted an unknown id") && ok;
  }

  // External notifications are unaffected.
  (void)manager.addOrReplace(NotificationRequest{.appName = "other-app", .summary = "external"});
  ok = check(historyContains(manager, "external"), "an external notification stopped being persisted") && ok;

  return ok ? 0 : 1;
}
