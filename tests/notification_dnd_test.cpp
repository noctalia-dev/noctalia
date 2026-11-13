#include "notification/notification_manager.h"
#include "tests/test_check.h"

#include <string>
#include <utility>

int main() {
  NotificationManager manager;
  int closeCallbacks = 0;
  int closedEvents = 0;

  manager.setCloseCallback([&closeCallbacks](uint32_t, CloseReason, const std::string&) { ++closeCallbacks; });
  manager.addEventCallback([&closedEvents](const Notification&, NotificationEvent event) {
    if (event == NotificationEvent::Closed) {
      ++closedEvents;
    }
  });

  const auto addNotification = [&manager](std::string summary) {
    return manager.addOrReplace(
        NotificationRequest{
            .appName = "dnd-test",
            .summary = std::move(summary),
            .timeout = 0,
            .transient = true,
        }
    );
  };

  const uint32_t firstId = addNotification("first");
  const uint32_t secondId = addNotification("second");

  TEST_CHECK(firstId != secondId);
  TEST_CHECK(manager.all().size() == 2);

  manager.setDoNotDisturb(true);
  TEST_CHECK(manager.doNotDisturb());
  TEST_CHECK(manager.all().size() == 2);
  TEST_CHECK(closeCallbacks == 0);
  TEST_CHECK(closedEvents == 0);

  manager.setDoNotDisturb(false);
  TEST_CHECK(!manager.doNotDisturb());
  TEST_CHECK(manager.all().size() == 2);
  TEST_CHECK(closeCallbacks == 0);
  TEST_CHECK(closedEvents == 0);

  manager.setDoNotDisturb(true);
  manager.setFilters({NotificationFilterConfig{
      .name = "medication",
      .match = "medication",
      .bypassDnd = true,
  }});
  const uint32_t bypassId = manager.addOrReplace(
      NotificationRequest{
          .appName = "Medication Reminder",
          .summary = "Take medication",
          .timeout = 0,
          .transient = true,
      }
  );
  TEST_CHECK(manager.all().back().id == bypassId);
  TEST_CHECK(manager.all().back().dndPolicy == NotificationDndPolicy::Bypass);

  const uint32_t ordinaryId = manager.addOrReplace(
      NotificationRequest{
          .appName = "Other",
          .summary = "Ordinary",
          .timeout = 0,
          .transient = true,
      }
  );
  TEST_CHECK(manager.all().back().id == ordinaryId);
  TEST_CHECK(manager.all().back().dndPolicy == NotificationDndPolicy::Respect);

  return 0;
}
