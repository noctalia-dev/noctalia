#include "calendar/calendar_reminders.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <print>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

  using namespace std::chrono;

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "calendar_reminders_test: {}", message);
    }
    return condition;
  }

  system_clock::time_point at(std::int64_t unixSeconds) { return system_clock::time_point{seconds{unixSeconds}}; }

  CalendarEvent timedEvent(std::string id, system_clock::time_point start, std::vector<std::int32_t> leads = {}) {
    CalendarEvent event;
    event.id = std::move(id);
    event.title = "Meeting";
    event.start = start;
    event.end = start + hours{1};
    event.reminderLeadSeconds = std::move(leads);
    return event;
  }

  CalendarSnapshot snapshotOf(std::vector<CalendarEvent> events) {
    CalendarSnapshot snapshot;
    snapshot.valid = true;
    snapshot.events = std::move(events);
    return snapshot;
  }

  CalendarConfig::Reminders defaultConfig() {
    CalendarConfig::Reminders config;
    config.enabled = true;
    config.useEventReminders = true;
    config.defaultLeadMinutes = 10;
    config.allDayDigestTime = ""; // digest exercised separately
    return config;
  }

} // namespace

int main() {
  bool ok = true;

  // ---- normalizeReminderLeads ----
  {
    std::vector<std::int32_t> leads{1800, -60, 300, 1800, calendar::kMaxLeadSeconds + 1, 0};
    calendar::normalizeReminderLeads(leads);
    ok = expect(leads == (std::vector<std::int32_t>{0, 300, 1800}), "normalize did not sort/dedupe/filter") && ok;
  }
  {
    std::vector<std::int32_t> leads;
    for (std::int32_t i = 1; i <= 10; ++i) {
      leads.push_back(i * 60);
    }
    calendar::normalizeReminderLeads(leads);
    ok = expect(leads.size() == calendar::kMaxRemindersPerEvent, "normalize did not cap the lead count") && ok;
  }

  // ---- reminderKey ----
  {
    const auto base = at(1'700'000'000);
    const CalendarEvent first = timedEvent("uid-1", base);
    const CalendarEvent second = timedEvent("uid-1", base + hours{24});
    ok = expect(
             calendar::reminderKey(first, 900) != calendar::reminderKey(second, 900),
             "recurrence instances of one UID collided on the same key"
         )
        && ok;
    ok = expect(
             calendar::reminderKey(first, 900) != calendar::reminderKey(first, 300),
             "different leads collided on the same key"
         )
        && ok;
    // The same event reached through two accounts must collapse to a single notification.
    CalendarEvent viaOtherAccount = first;
    viaOtherAccount.calendarName = "Work";
    viaOtherAccount.colorHex = "#ff0000";
    ok = expect(
             calendar::reminderKey(first, 900) == calendar::reminderKey(viaOtherAccount, 900),
             "the same event via two accounts did not share a key"
         )
        && ok;
    const auto start = calendar::reminderKeyStart(calendar::reminderKey(first, 900));
    ok = expect(start.has_value() && *start == first.start, "reminderKeyStart did not round-trip") && ok;
  }

  // ---- pruneFiredKeys ----
  {
    const auto now = at(1'700'000'000);
    const CalendarEvent past = timedEvent("past", now - hours{1});
    const CalendarEvent future = timedEvent("future", now + hours{1});
    std::unordered_set<std::string> fired{
        calendar::reminderKey(past, 900), calendar::reminderKey(future, 900), "garbage"
    };
    calendar::pruneFiredKeys(fired, now);
    ok = expect(
             fired.size() == 1 && fired.contains(calendar::reminderKey(future, 900)),
             "prune did not retain exactly the not-yet-started key"
         )
        && ok;
  }

  // ---- planReminders: explicit lead beats the configured default ----
  {
    const auto now = at(1'700'000'000);
    const auto snapshot = snapshotOf({timedEvent("a", now + minutes{15}, {900})});
    const auto plan = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    ok = expect(plan.due.size() == 1, "explicit 15 minute reminder was not due") && ok;
    ok =
        expect(!plan.due.empty() && !plan.due.front().fromDefaultLead, "explicit lead was reported as a default") && ok;
  }

  // ---- planReminders: default lead applies when the event carries none ----
  {
    const auto now = at(1'700'000'000);
    const auto snapshot = snapshotOf({timedEvent("a", now + minutes{9})});
    const auto plan = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    ok = expect(plan.due.size() == 1 && plan.due.front().fromDefaultLead, "default lead did not fire") && ok;
  }

  // ---- changing the default lead must not re-notify an already fired reminder ----
  {
    const auto now = at(1'700'000'000);
    const auto snapshot = snapshotOf({timedEvent("a", now + minutes{9})});
    auto config = defaultConfig();
    const auto first = calendar::planReminders(snapshot, config, {}, std::nullopt, now);
    ok = expect(first.due.size() == 1, "first pass did not fire the default reminder") && ok;

    std::unordered_set<std::string> fired;
    for (const auto& due : first.due) {
      fired.insert(due.key);
    }
    config.defaultLeadMinutes = 15; // user moves the slider
    const auto second = calendar::planReminders(snapshot, config, fired, std::nullopt, now);
    ok = expect(second.due.empty(), "changing the default lead re-notified an already fired reminder") && ok;
  }

  // ---- an already fired key is never re-emitted ----
  {
    const auto now = at(1'700'000'000);
    const CalendarEvent event = timedEvent("a", now + minutes{5}, {900});
    const auto snapshot = snapshotOf({event});
    std::unordered_set<std::string> fired{calendar::reminderKey(event, 900)};
    const auto plan = calendar::planReminders(snapshot, defaultConfig(), fired, std::nullopt, now);
    ok = expect(plan.due.empty(), "an already fired reminder was re-emitted") && ok;
  }

  // ---- a reminder whose event already started is dropped ----
  {
    const auto now = at(1'700'000'000);
    const auto snapshot = snapshotOf({timedEvent("a", now - minutes{1}, {900})});
    const auto plan = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    ok = expect(plan.due.empty(), "a reminder for an already started event fired") && ok;
  }

  // ---- a missed reminder for a still future event fires exactly once ----
  {
    const auto now = at(1'700'000'000);
    const CalendarEvent event = timedEvent("a", now + minutes{5}, {1800}); // due 25 minutes ago
    const auto snapshot = snapshotOf({event});
    const auto first = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    ok = expect(first.due.size() == 1, "a missed but still relevant reminder did not fire") && ok;

    std::unordered_set<std::string> fired;
    for (const auto& due : first.due) {
      fired.insert(due.key);
    }
    const auto second = calendar::planReminders(snapshot, defaultConfig(), fired, std::nullopt, now);
    ok = expect(second.due.empty(), "a missed reminder fired twice") && ok;
  }

  // ---- an at-start reminder (TRIGGER:PT0S) must fire ----
  {
    const auto now = at(1'700'000'000);
    // Due exactly now: a "notify at the moment it starts" alarm.
    const auto snapshot = snapshotOf({timedEvent("a", now, {0})});
    const auto plan = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    ok = expect(plan.due.size() == 1, "an at-start (lead 0) reminder never fired") && ok;
  }

  // ---- an at-start reminder discovered slightly late still fires once, then stops ----
  {
    const auto now = at(1'700'000'000);
    const CalendarEvent event = timedEvent("a", now - minutes{1}, {0});
    const auto snapshot = snapshotOf({event});
    const auto plan = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    ok = expect(plan.due.size() == 1, "an at-start reminder one minute late did not fire") && ok;

    std::unordered_set<std::string> fired{calendar::reminderKey(event, 0)};
    const auto repeat = calendar::planReminders(snapshot, defaultConfig(), fired, std::nullopt, now);
    ok = expect(repeat.due.empty(), "an at-start reminder fired twice") && ok;
  }

  // ---- a long-past at-start reminder is not resurrected ----
  {
    const auto now = at(1'700'000'000);
    const auto snapshot = snapshotOf({timedEvent("a", now - hours{2}, {0})});
    const auto plan = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    ok = expect(plan.due.empty(), "a long-past at-start reminder was resurrected") && ok;
  }

  // ---- pruning must not drop a key while its reminder can still re-fire ----
  {
    const auto now = at(1'700'000'000);
    const CalendarEvent event = timedEvent("a", now, {0});
    std::unordered_set<std::string> fired{calendar::reminderKey(event, 0)};
    calendar::pruneFiredKeys(fired, now);
    ok = expect(!fired.empty(), "prune dropped a just-fired at-start key, allowing a repeat") && ok;
  }

  // ---- several reminders coming due at once collapse to one notification per event ----
  {
    const auto now = at(1'700'000'000);
    // Cold start: both the 30 and 10 minute reminders for this meeting are already past due.
    const auto snapshot = snapshotOf({timedEvent("a", now + minutes{5}, {600, 1800})});
    const auto plan = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    const auto notifying = std::ranges::count_if(plan.due, [](const calendar::DueReminder& d) { return d.notify; });
    ok = expect(plan.due.size() == 2, "both due reminders should be recorded as fired") && ok;
    ok = expect(notifying == 1, "two reminders for one event produced two notifications") && ok;
    // The one shown must be the closest to the event, so the wording is accurate.
    const auto* shown = std::ranges::find_if(plan.due, [](const calendar::DueReminder& d) { return d.notify; }).base();
    ok = expect(shown != nullptr && shown->leadSeconds == 600, "the collapsed notification used the wrong lead") && ok;
  }

  // ---- reminders coming due at different times still notify separately ----
  {
    const auto now = at(1'700'000'000);
    const CalendarEvent event = timedEvent("a", now + minutes{25}, {600, 1800});
    const auto snapshot = snapshotOf({event});
    const auto first = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    ok = expect(first.due.size() == 1 && first.due.front().notify, "the 30 minute reminder did not fire on time") && ok;

    std::unordered_set<std::string> fired{calendar::reminderKey(event, 1800)};
    const auto second = calendar::planReminders(snapshot, defaultConfig(), fired, std::nullopt, now + minutes{15});
    ok = expect(second.due.size() == 1 && second.due.front().notify, "the 10 minute reminder did not fire later") && ok;
  }

  // ---- the same meeting from two accounts with differing UIDs notifies once ----
  {
    const auto now = at(1'700'000'000);
    // Providers often disagree on the UID for the same meeting, so reminderKey cannot merge these.
    auto viaGoogle = timedEvent("google-abc123", now + minutes{5}, {600});
    auto viaCaldav = timedEvent("caldav-xyz789@example.com", now + minutes{5}, {600});
    viaGoogle.calendarName = "Work";
    viaCaldav.calendarName = "Personal";
    const auto plan =
        calendar::planReminders(snapshotOf({viaGoogle, viaCaldav}), defaultConfig(), {}, std::nullopt, now);
    const auto notifying = std::ranges::count_if(plan.due, [](const calendar::DueReminder& d) { return d.notify; });
    ok = expect(plan.due.size() == 2, "both account copies should be recorded as fired") && ok;
    ok = expect(notifying == 1, "the same meeting from two accounts notified twice") && ok;
  }

  // ---- distinct meetings at the same time still notify separately ----
  {
    const auto now = at(1'700'000'000);
    const auto a = timedEvent("a", now + minutes{5}, {600});
    auto b = timedEvent("b", now + minutes{5}, {600});
    b.title = "Other meeting";
    const auto plan = calendar::planReminders(snapshotOf({a, b}), defaultConfig(), {}, std::nullopt, now);
    const auto notifying = std::ranges::count_if(plan.due, [](const calendar::DueReminder& d) { return d.notify; });
    ok = expect(notifying == 2, "two different meetings at the same time were collapsed") && ok;
  }

  // ---- all-day events never produce per-event reminders ----
  {
    const auto now = at(1'700'000'000);
    CalendarEvent allDay = timedEvent("allday", now + hours{2}, {900});
    allDay.allDay = true;
    const auto plan = calendar::planReminders(snapshotOf({allDay}), defaultConfig(), {}, std::nullopt, now);
    ok = expect(plan.due.empty(), "an all-day event produced a per-event reminder") && ok;
  }

  // ---- nextWake is the earliest future due time ----
  {
    const auto now = at(1'700'000'000);
    const auto snapshot = snapshotOf({
        timedEvent("a", now + hours{2}, {900}),  // due in 1h45m
        timedEvent("b", now + hours{1}, {1800}), // due in 30m
    });
    const auto plan = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    ok = expect(plan.due.empty(), "a future reminder fired early") && ok;
    ok = expect(plan.nextWake.has_value() && *plan.nextWake == now + minutes{30}, "nextWake was not the earliest due")
        && ok;
  }

  // ---- disabled reminders produce nothing and arm no wakeup ----
  {
    const auto now = at(1'700'000'000);
    auto config = defaultConfig();
    config.enabled = false;
    const auto plan =
        calendar::planReminders(snapshotOf({timedEvent("a", now + minutes{5}, {900})}), config, {}, std::nullopt, now);
    ok = expect(plan.due.empty() && !plan.nextWake.has_value(), "disabled reminders still planned work") && ok;
  }

  // ---- an invalid snapshot is ignored ----
  {
    const auto now = at(1'700'000'000);
    CalendarSnapshot snapshot;
    snapshot.events = {timedEvent("a", now + minutes{5}, {900})};
    snapshot.valid = false;
    const auto plan = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    ok = expect(plan.due.empty(), "an invalid snapshot produced reminders") && ok;
  }

  // ---- digest: fires once per local date, and still fires when the shell starts late ----
  {
    const auto now = system_clock::now();
    auto config = defaultConfig();
    // A digest time already past today, so it is due right now.
    config.allDayDigestTime = "00:00";

    CalendarEvent allDay;
    allDay.id = "birthday";
    allDay.title = "Birthday";
    allDay.allDay = true;
    const auto today = calendar::digestInstantFor(now, "00:00");
    ok = expect(today.has_value(), "digestInstantFor returned no instant for 00:00") && ok;
    if (today.has_value()) {
      allDay.start = *today;
      allDay.end = *today + hours{24};

      const auto plan = calendar::planReminders(snapshotOf({allDay}), config, {}, std::nullopt, now);
      ok = expect(plan.digestDue && plan.digest.size() == 1, "the all-day digest did not fire") && ok;

      const auto repeat = calendar::planReminders(snapshotOf({allDay}), config, {}, calendar::localDateKey(now), now);
      ok = expect(!repeat.digestDue, "the all-day digest fired twice on the same date") && ok;
    }
  }

  // ---- digest: an empty day does not mark the date as handled ----
  {
    const auto now = system_clock::now();
    auto config = defaultConfig();
    config.allDayDigestTime = "00:00";
    const auto plan = calendar::planReminders(snapshotOf({}), config, {}, std::nullopt, now);
    ok = expect(!plan.digestDue, "an empty digest was reported as due") && ok;
  }

  // ---- digestInstantFor validates its input ----
  {
    const auto now = system_clock::now();
    ok = expect(!calendar::digestInstantFor(now, "").has_value(), "an empty digest time was accepted") && ok;
    ok = expect(!calendar::digestInstantFor(now, "9:00").has_value(), "an unpadded digest time was accepted") && ok;
    ok = expect(!calendar::digestInstantFor(now, "24:00").has_value(), "an out of range hour was accepted") && ok;
    ok = expect(calendar::digestInstantFor(now, "09:30").has_value(), "a valid digest time was rejected") && ok;
  }

  // ---- countdown: rounds up, and every step sits on a whole minute before the event ----
  {
    const auto start = at(1'700'000'000);
    // A reminder fires a hair after its due instant, which is the case that used to render every
    // ten-minute lead as "in 9 min".
    ok = expect(
             calendar::countdownMinutes(start, start - seconds{600} + milliseconds{7}) == 10,
             "a reminder firing just after its due instant did not read 10 minutes"
         )
        && ok;
    ok = expect(
             calendar::countdownMinutes(start, start - seconds{600}) == 10, "an exact ten-minute lead did not read 10"
         )
        && ok;
    // The label must change exactly nine minutes before the event, not eight and a half.
    ok = expect(calendar::countdownMinutes(start, start - seconds{541}) == 10, "9m01s did not still read 10") && ok;
    ok = expect(calendar::countdownMinutes(start, start - seconds{540}) == 9, "an exact nine minutes did not read 9")
        && ok;
    // And "starting now" belongs to the start itself, not to the last half minute before it.
    ok = expect(calendar::countdownMinutes(start, start - seconds{1}) == 1, "one second out already read 0") && ok;
    ok = expect(calendar::countdownMinutes(start, start) == 0, "the event start did not read 0") && ok;
    ok = expect(calendar::countdownMinutes(start, start + seconds{90}) <= 0, "a started event reported minutes left")
        && ok;
  }

  // ---- countdown: each wake observes a lower value, and the last one settles ----
  {
    const auto start = at(1'700'000'000);
    auto now = start - seconds{600};
    std::int64_t shown = calendar::countdownMinutes(start, now);
    ok = expect(shown == 10, "the countdown did not start at 10") && ok;

    int steps = 0;
    while (auto next = calendar::countdownNextChange(start, now)) {
      ok = expect(*next > now, "the next countdown change was not strictly in the future") && ok;
      now = *next;
      const std::int64_t updated = calendar::countdownMinutes(start, now);
      ok = expect(updated < shown, "a countdown wake did not lower the displayed minutes") && ok;
      shown = updated;
      if (++steps > 20) {
        break;
      }
    }
    ok = expect(steps == 10, "the countdown from 10 minutes out did not take 10 steps to settle") && ok;
    ok = expect(shown <= 0, "the countdown settled on something other than \"starts now\"") && ok;
    ok = expect(now == start, "the countdown did not settle exactly at the event start") && ok;
  }

  // ---- countdown: a far-out reminder waits instead of ticking all day ----
  {
    const auto start = at(1'700'000'000);
    const auto next = calendar::countdownNextChange(start, start - hours{24});
    ok = expect(
             next.has_value() && *next == start - calendar::kCountdownWindow,
             "a day-ahead reminder armed a per-minute wake instead of waiting for the window"
         )
        && ok;
    ok = expect(!calendar::countdownNextChange(start, start).has_value(), "a started event still armed a wake") && ok;
  }

  return ok ? 0 : 1;
}
