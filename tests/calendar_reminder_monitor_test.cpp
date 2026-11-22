// Drives the real monitor against a real NotificationManager to prove the countdown state machine
// terminates: a reminder that is counting down arms exactly one poll deadline, and once it settles or
// is dismissed the monitor advertises no deadline at all. There are no threads or timer objects here,
// so a leak would show up as a poll deadline that never goes away, or one that never moves forward.
#include "calendar/calendar_reminder_monitor.h"
#include "calendar/calendar_types.h"
#include "config/config_service.h"
#include "i18n/i18n_service.h"
#include "notification/notification_manager.h"
#include "tests/test_check.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

  std::filesystem::path gRoot;

  void removeRoot() { std::filesystem::remove_all(gRoot); }

  CalendarSnapshot
  snapshotWithEvent(std::string id, std::chrono::system_clock::time_point start, std::int32_t leadSeconds) {
    CalendarEvent event;
    event.id = std::move(id);
    event.title = "Standup";
    event.start = start;
    event.end = start + std::chrono::hours{1};
    event.reminderLeadSeconds = {leadSeconds};

    CalendarSnapshot snapshot;
    snapshot.valid = true;
    snapshot.events.push_back(std::move(event));
    return snapshot;
  }

  // The monitor is the only thing adding notifications here, so the newest one is the reminder.
  std::uint32_t newestNotificationId(const NotificationManager& manager) {
    return manager.all().empty() ? 0 : manager.all().back().id;
  }

} // namespace

int main() {
  gRoot = std::filesystem::temp_directory_path() / ("noctalia-reminder-monitor-test-" + std::to_string(::getpid()));
  const auto configHome = gRoot / "config";
  const auto stateHome = gRoot / "state";
  const auto cacheHome = gRoot / "cache";
  const auto configDir = configHome / "noctalia";

  std::filesystem::remove_all(gRoot);
  std::atexit(removeRoot);
  std::filesystem::create_directories(configDir);
  std::filesystem::create_directories(stateHome);
  std::filesystem::create_directories(cacheHome);
  ::setenv("NOCTALIA_CONFIG_HOME", configHome.c_str(), 1);
  ::setenv("NOCTALIA_STATE_HOME", stateHome.c_str(), 1);
  ::setenv("XDG_CACHE_HOME", cacheHome.c_str(), 1);

  {
    // An empty digest time keeps the once-a-day digest out of the picture, so every poll deadline the
    // monitor advertises belongs to a reminder.
    std::ofstream config(configDir / "config.toml");
    config << "[calendar]\nenabled = true\n\n[calendar.reminders]\nenabled = true\nall_day_digest_time = \"\"\n";
  }
  i18n::Service::instance().init("en");

  using namespace std::chrono;

  // ---- a reminder still counting down arms one deadline, and dismissing it clears the deadline ----
  {
    ConfigService config;
    NotificationManager notifications;
    CalendarReminderMonitor monitor(config, notifications);
    monitor.initialize();

    // Due right now, with a couple of minutes of countdown left to run. The lead deliberately avoids
    // a whole- or half-minute value, which would sit on a rounding boundary and make the first step
    // arbitrary.
    const auto start = system_clock::now() + seconds{100};
    const CalendarSnapshot snapshot = snapshotWithEvent("uid-counting", start, 100);
    monitor.onSnapshotChanged(snapshot);

    const std::uint32_t id = newestNotificationId(notifications);
    TEST_CHECK(id != 0);

    const int armed = monitor.pollTimeoutMs();
    // Strictly in the future, or the poll loop would spin at a zero timeout instead of sleeping, and
    // no further out than one countdown step plus the second the wake deliberately overshoots by.
    TEST_CHECK(armed > 0);
    TEST_CHECK(armed <= 60 * 1000);

    TEST_CHECK(notifications.close(id, CloseReason::Dismissed));
    monitor.tick();
    // The monitor kept waking up for a reminder the user had already dismissed.
    TEST_CHECK(monitor.pollTimeoutMs() == -1);

    // And it stays quiet: nothing re-arms it on a later pass.
    monitor.tick();
    monitor.onSnapshotChanged(snapshot);
    TEST_CHECK(monitor.pollTimeoutMs() == -1);
  }

  // ---- a reminder that fires with nothing left to count down never arms anything ----
  {
    ConfigService config;
    NotificationManager notifications;
    CalendarReminderMonitor monitor(config, notifications);
    monitor.initialize();

    // An at-start reminder for an event that has just begun: the label reads "starts now" the moment
    // it appears and can never change again.
    // The snapshot must outlive the call: the monitor keeps a pointer to the one CalendarService owns.
    const CalendarSnapshot snapshot = snapshotWithEvent("uid-at-start", system_clock::now() - seconds{10}, 0);
    monitor.onSnapshotChanged(snapshot);

    TEST_CHECK(newestNotificationId(notifications) != 0);
    // A reminder with a settled label still armed a poll deadline.
    TEST_CHECK(monitor.pollTimeoutMs() == -1);
  }

  // ---- turning reminders off stops the wakeups ----
  {
    ConfigService config;
    NotificationManager notifications;
    CalendarReminderMonitor monitor(config, notifications);
    monitor.initialize();

    const CalendarSnapshot snapshot = snapshotWithEvent("uid-disabled", system_clock::now() + seconds{100}, 100);
    monitor.onSnapshotChanged(snapshot);
    TEST_CHECK(monitor.pollTimeoutMs() > 0);

    {
      std::ofstream disabled(configDir / "config.toml");
      disabled << "[calendar]\nenabled = true\n\n[calendar.reminders]\nenabled = false\n";
    }
    config.forceReload();
    monitor.tick();
    // Disabling reminders left the monitor waking up.
    TEST_CHECK(monitor.pollTimeoutMs() == -1);
  }

  return 0;
}
