#include "notification/notification_manager.h"

#include <iostream>
#include <optional>
#include <string>

namespace {

  bool check(bool condition, const char* message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
  }

  const Notification* findNotification(const NotificationManager& manager, uint32_t id) {
    for (const auto& notification : manager.all()) {
      if (notification.id == id) {
        return &notification;
      }
    }
    return nullptr;
  }

  uint32_t addExternal(
      NotificationManager& manager, std::string summary, Urgency urgency, int32_t timeout, uint32_t replacesId = 0
  ) {
    return manager.addOrReplace(
        NotificationRequest{
            .replacesId = replacesId,
            .appName = "timeout-test",
            .summary = std::move(summary),
            .urgency = urgency,
            .timeout = timeout,
            .origin = NotificationOrigin::External,
            .transient = true,
        }
    );
  }

  NotificationTimeoutConfig urgencyPolicy() {
    return NotificationTimeoutConfig{
        .mode = NotificationTimeoutMode::Urgency,
        .low = 2000,
        .normal = 6000,
        .critical = 15000,
    };
  }

} // namespace

int main() {
  bool ok = true;

  {
    NotificationManager manager;
    const uint32_t positiveId = addExternal(manager, "requested-positive", Urgency::Normal, 3200);
    const uint32_t persistentId = addExternal(manager, "requested-persistent", Urgency::Normal, 0);
    const uint32_t defaultId = addExternal(manager, "requested-default", Urgency::Normal, -1);

    ok &= check(findNotification(manager, positiveId)->timeout == 3200, "requested mode keeps a positive timeout");
    ok &= check(findNotification(manager, persistentId)->timeout == 0, "requested mode keeps timeout zero persistent");
    ok &= check(
        findNotification(manager, defaultId)->timeout == kDefaultNotificationTimeout,
        "requested mode resolves the sender default"
    );
  }

  {
    NotificationManager manager;
    manager.setTimeoutPolicy(urgencyPolicy());

    const uint32_t lowId = addExternal(manager, "urgency-low", Urgency::Low, 900);
    const uint32_t normalId = addExternal(manager, "urgency-normal", Urgency::Normal, -1);
    const uint32_t criticalId = manager.adoptExternal(
        42,
        NotificationRequest{
            .appName = "timeout-test",
            .summary = "urgency-critical",
            .urgency = Urgency::Critical,
            .timeout = 900,
            .transient = true,
        }
    );
    const uint32_t persistentId = addExternal(manager, "urgency-persistent", Urgency::Critical, 0);
    const uint32_t internalId = manager.addInternal(
        "timeout-test", "internal", "", Urgency::Critical, 4700, std::nullopt, std::nullopt, std::nullopt, std::nullopt
    );

    ok &= check(findNotification(manager, lowId)->timeout == 2000, "urgency mode uses the low duration");
    ok &= check(findNotification(manager, normalId)->timeout == 6000, "urgency mode uses the normal duration");
    ok &= check(findNotification(manager, criticalId)->timeout == 15000, "urgency mode uses the critical duration");
    ok &= check(findNotification(manager, persistentId)->timeout == 0, "urgency mode preserves sender timeout zero");
    ok &= check(
        findNotification(manager, internalId)->timeout == 4700, "urgency mode does not affect internal notifications"
    );
  }

  {
    NotificationManager manager;
    manager.setTimeoutPolicy(urgencyPolicy());
    manager.setFilters({NotificationFilterConfig{
        .name = "timeout-test",
        .match = "timeout-test",
        .overrideDuration = 9100,
    }});

    const uint32_t id = addExternal(manager, "filter-override", Urgency::Low, 0);
    ok &= check(findNotification(manager, id)->timeout == 9100, "filter duration overrides urgency policy");
  }

  {
    NotificationManager manager;
    const uint32_t id = addExternal(manager, "replacement-before-reload", Urgency::Low, 3100);
    const Notification* before = findNotification(manager, id);
    ok &= check(before != nullptr && before->timeout == 3100, "initial notification uses requested mode");
    const std::optional<TimePoint> expiryBefore = before == nullptr ? std::nullopt : before->expiryTime;

    manager.setTimeoutPolicy(urgencyPolicy());
    const Notification* unchanged = findNotification(manager, id);
    ok &=
        check(unchanged != nullptr && unchanged->timeout == 3100, "policy reload leaves an existing timeout unchanged");
    ok &= check(
        unchanged != nullptr && unchanged->expiryTime == expiryBefore,
        "policy reload leaves an existing expiry timer unchanged"
    );

    const uint32_t replacementId = addExternal(manager, "replacement-after-reload", Urgency::Critical, 3100, id);
    const Notification* replacement = findNotification(manager, replacementId);
    ok &= check(replacementId == id, "replacement retains its notification id");
    ok &= check(replacement != nullptr && replacement->timeout == 15000, "replacement uses the current urgency policy");
  }

  return ok ? 0 : 1;
}
