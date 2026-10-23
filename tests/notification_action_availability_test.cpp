#include "notification/notification_manager.h"
#include "tests/test_check.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

  uint32_t addNotification(NotificationManager& manager, std::vector<std::string> actions) {
    static unsigned int sequence = 0;
    return manager.addOrReplace(
        NotificationRequest{
            .appName = "action-availability-test",
            .summary = "notification-" + std::to_string(++sequence),
            .timeout = 0,
            .actions = std::move(actions),
        }
    );
  }

  void checkLiveAndExpiredActions() {
    NotificationManager manager;
    std::vector<std::string> invocations;
    manager.setActionInvokeCallback([&invocations](uint32_t, const std::string& key, const std::string& token) {
      invocations.push_back(key + ":" + token);
    });

    const uint32_t live = addNotification(manager, {"default", "Open"});
    TEST_CHECK(!manager.hasPendingDBusClose(live));
    TEST_CHECK(manager.canInvokeAction(live, "default"));
    TEST_CHECK(invocations.empty());
    TEST_CHECK(manager.invokeAction(live, "default", "live-token", true));
    TEST_CHECK(invocations == std::vector<std::string>{"default:live-token"});
    TEST_CHECK(!manager.canInvokeAction(live, "default"));
    TEST_CHECK(!manager.invokeAction(live, "default"));

    const uint32_t expired = addNotification(manager, {"default", "Open"});
    TEST_CHECK(manager.close(expired, CloseReason::Expired));
    TEST_CHECK(manager.hasPendingDBusClose(expired));
    TEST_CHECK(manager.canInvokeAction(expired, "default"));
    TEST_CHECK(manager.invokeAction(expired, "default", "history-token", true));
    TEST_CHECK(invocations.size() == 2);
    TEST_CHECK(invocations.back() == "default:history-token");
    TEST_CHECK(!manager.canInvokeAction(expired, "default"));
    TEST_CHECK(!manager.invokeAction(expired, "default"));
    TEST_CHECK(invocations.size() == 2);
  }

  void checkWithdrawnAndRemovedHistory() {
    NotificationManager manager;
    const uint32_t withdrawn = addNotification(manager, {"default", "Open"});
    TEST_CHECK(manager.close(withdrawn, CloseReason::ClosedByCall));
    TEST_CHECK(!manager.history().empty());
    TEST_CHECK(!manager.history().back().notification.actions.empty());
    TEST_CHECK(!manager.canInvokeAction(withdrawn, "default"));

    const uint32_t expired = addNotification(manager, {"default", "Open"});
    TEST_CHECK(manager.close(expired, CloseReason::Expired));
    TEST_CHECK(manager.canInvokeAction(expired, "default"));
    TEST_CHECK(manager.close(expired, CloseReason::ClosedByCall));
    TEST_CHECK(!manager.canInvokeAction(expired, "default"));

    const uint32_t removed = addNotification(manager, {"default", "Open"});
    TEST_CHECK(manager.close(removed, CloseReason::Expired));
    manager.removeHistoryEntry(removed);
    TEST_CHECK(!manager.canInvokeAction(removed, "default"));

    const uint32_t cleared = addNotification(manager, {"default", "Open"});
    TEST_CHECK(manager.close(cleared, CloseReason::Expired));
    manager.clearHistory();
    TEST_CHECK(!manager.canInvokeAction(cleared, "default"));
  }

  void checkDeclaredActionKeys() {
    NotificationManager manager;
    const uint32_t named = addNotification(manager, {"archive", "default", "inline-reply", "Reply"});
    TEST_CHECK(manager.canInvokeAction(named, "archive"));
    TEST_CHECK(!manager.canInvokeAction(named, "default"));
    TEST_CHECK(!manager.canInvokeAction(named, "inline-reply"));
    TEST_CHECK(!manager.canInvokeAction(named, ""));
    TEST_CHECK(!manager.canInvokeAction(named, "unknown"));
    TEST_CHECK(!manager.canInvokeAction(0, "default"));

    const uint32_t incomplete = addNotification(manager, {"default"});
    TEST_CHECK(!manager.canInvokeAction(incomplete, "default"));

    const uint32_t replaced = addNotification(manager, {"default", "Open"});
    TEST_CHECK(manager.canInvokeAction(replaced, "default"));
    TEST_CHECK(
        manager.addOrReplace(
            NotificationRequest{
                .replacesId = replaced,
                .appName = "action-availability-test",
                .summary = "updated",
                .timeout = 0,
                .actions = {"archive", "Archive"},
            }
        )
        == replaced
    );
    TEST_CHECK(!manager.canInvokeAction(replaced, "default"));
    TEST_CHECK(manager.canInvokeAction(replaced, "archive"));
  }

} // namespace

int main() {
  checkLiveAndExpiredActions();
  checkWithdrawnAndRemovedHistory();
  checkDeclaredActionKeys();
  return 0;
}
