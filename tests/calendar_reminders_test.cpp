#include "calendar/calendar_reminders.h"
#include "tests/test_check.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

  using namespace std::chrono;

  system_clock::time_point at(std::int64_t unixSeconds) { return system_clock::time_point{seconds{unixSeconds}}; }

  // An event whose source says nothing about reminders, so the configured default lead applies.
  CalendarEvent timedEvent(std::string id, system_clock::time_point start) {
    CalendarEvent event;
    event.id = std::move(id);
    event.title = "Meeting";
    event.start = start;
    event.end = start + hours{1};
    return event;
  }

  // An event carrying explicit reminder leads; an empty list means "explicitly no reminder".
  CalendarEvent timedEvent(std::string id, system_clock::time_point start, std::vector<std::int32_t> leads) {
    CalendarEvent event = timedEvent(std::move(id), start);
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
  // ---- normalizeReminderLeads ----
  {
    std::vector<std::int32_t> leads{1800, -60, 300, 1800, calendar::kMaxLeadSeconds + 1, 0};
    calendar::normalizeReminderLeads(leads);
    TEST_CHECK(leads == (std::vector<std::int32_t>{0, 300, 1800}));
  }
  {
    std::vector<std::int32_t> leads;
    for (std::int32_t i = 1; i <= 10; ++i) {
      leads.push_back(i * 60);
    }
    calendar::normalizeReminderLeads(leads);
    TEST_CHECK(leads.size() == calendar::kMaxRemindersPerEvent);
  }

  // ---- reminderKey ----
  {
    const auto base = at(1'700'000'000);
    const CalendarEvent first = timedEvent("uid-1", base);
    const CalendarEvent second = timedEvent("uid-1", base + hours{24});
    TEST_CHECK(calendar::reminderKey(first, 900) != calendar::reminderKey(second, 900));
    TEST_CHECK(calendar::reminderKey(first, 900) != calendar::reminderKey(first, 300));
    // The same event reached through two accounts must collapse to a single notification.
    CalendarEvent viaOtherAccount = first;
    viaOtherAccount.calendarName = "Work";
    viaOtherAccount.colorHex = "#ff0000";
    TEST_CHECK(calendar::reminderKey(first, 900) == calendar::reminderKey(viaOtherAccount, 900));
    const auto start = calendar::reminderKeyStart(calendar::reminderKey(first, 900));
    TEST_CHECK(start.has_value() && *start == first.start);
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
    TEST_CHECK(fired.size() == 1 && fired.contains(calendar::reminderKey(future, 900)));
  }

  // ---- planReminders: explicit lead beats the configured default ----
  {
    const auto now = at(1'700'000'000);
    const auto snapshot = snapshotOf({timedEvent("a", now + minutes{15}, {900})});
    const auto plan = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    TEST_CHECK(plan.due.size() == 1);
    TEST_CHECK(!plan.due.front().fromDefaultLead);
  }

  // ---- planReminders: default lead applies when the event carries none ----
  {
    const auto now = at(1'700'000'000);
    const auto snapshot = snapshotOf({timedEvent("a", now + minutes{9})});
    const auto plan = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    TEST_CHECK(plan.due.size() == 1 && plan.due.front().fromDefaultLead);
  }

  // ---- changing the default lead must not re-notify an already fired reminder ----
  {
    const auto now = at(1'700'000'000);
    const auto snapshot = snapshotOf({timedEvent("a", now + minutes{9})});
    auto config = defaultConfig();
    const auto first = calendar::planReminders(snapshot, config, {}, std::nullopt, now);
    TEST_CHECK(first.due.size() == 1);

    std::unordered_set<std::string> fired;
    for (const auto& due : first.due) {
      fired.insert(due.key);
    }
    config.defaultLeadMinutes = 15; // user moves the slider
    const auto second = calendar::planReminders(snapshot, config, fired, std::nullopt, now);
    TEST_CHECK(second.due.empty());
  }

  // ---- an explicit "no reminder" stays silent, unless event reminders are turned off ----
  {
    const auto now = at(1'700'000'000);
    const auto snapshot = snapshotOf({timedEvent("a", now + minutes{5}, std::vector<std::int32_t>{})});
    const auto silent = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    TEST_CHECK(silent.due.empty() && !silent.nextWake.has_value());

    auto config = defaultConfig();
    config.useEventReminders = false;
    const auto forced = calendar::planReminders(snapshot, config, {}, std::nullopt, now);
    TEST_CHECK(forced.due.size() == 1 && forced.due.front().fromDefaultLead);
  }

  // ---- an already fired key is never re-emitted ----
  {
    const auto now = at(1'700'000'000);
    const CalendarEvent event = timedEvent("a", now + minutes{5}, {900});
    const auto snapshot = snapshotOf({event});
    std::unordered_set<std::string> fired{calendar::reminderKey(event, 900)};
    const auto plan = calendar::planReminders(snapshot, defaultConfig(), fired, std::nullopt, now);
    TEST_CHECK(plan.due.empty());
  }

  // ---- a reminder whose event already started is dropped ----
  {
    const auto now = at(1'700'000'000);
    const auto snapshot = snapshotOf({timedEvent("a", now - minutes{1}, {900})});
    const auto plan = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    TEST_CHECK(plan.due.empty());
  }

  // ---- a missed reminder for a still future event fires exactly once ----
  {
    const auto now = at(1'700'000'000);
    const CalendarEvent event = timedEvent("a", now + minutes{5}, {1800}); // due 25 minutes ago
    const auto snapshot = snapshotOf({event});
    const auto first = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    TEST_CHECK(first.due.size() == 1);

    std::unordered_set<std::string> fired;
    for (const auto& due : first.due) {
      fired.insert(due.key);
    }
    const auto second = calendar::planReminders(snapshot, defaultConfig(), fired, std::nullopt, now);
    TEST_CHECK(second.due.empty());
  }

  // ---- an at-start reminder (TRIGGER:PT0S) must fire ----
  {
    const auto now = at(1'700'000'000);
    // Due exactly now: a "notify at the moment it starts" alarm.
    const auto snapshot = snapshotOf({timedEvent("a", now, {0})});
    const auto plan = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    TEST_CHECK(plan.due.size() == 1);
  }

  // ---- an at-start reminder discovered slightly late still fires once, then stops ----
  {
    const auto now = at(1'700'000'000);
    const CalendarEvent event = timedEvent("a", now - minutes{1}, {0});
    const auto snapshot = snapshotOf({event});
    const auto plan = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    TEST_CHECK(plan.due.size() == 1);

    std::unordered_set<std::string> fired{calendar::reminderKey(event, 0)};
    const auto repeat = calendar::planReminders(snapshot, defaultConfig(), fired, std::nullopt, now);
    TEST_CHECK(repeat.due.empty());
  }

  // ---- a long-past at-start reminder is not resurrected ----
  {
    const auto now = at(1'700'000'000);
    const auto snapshot = snapshotOf({timedEvent("a", now - hours{2}, {0})});
    const auto plan = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    TEST_CHECK(plan.due.empty());
  }

  // ---- pruning must not drop a key while its reminder can still re-fire ----
  {
    const auto now = at(1'700'000'000);
    const CalendarEvent event = timedEvent("a", now, {0});
    std::unordered_set<std::string> fired{calendar::reminderKey(event, 0)};
    calendar::pruneFiredKeys(fired, now);
    TEST_CHECK(!fired.empty());
  }

  // ---- several reminders coming due at once collapse to one notification per event ----
  {
    const auto now = at(1'700'000'000);
    // Cold start: both the 30 and 10 minute reminders for this meeting are already past due.
    const auto snapshot = snapshotOf({timedEvent("a", now + minutes{5}, {600, 1800})});
    const auto plan = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    const auto notifying = std::ranges::count_if(plan.due, [](const calendar::DueReminder& d) { return d.notify; });
    TEST_CHECK(plan.due.size() == 2);
    TEST_CHECK(notifying == 1);
    // The one shown must be the closest to the event, so the wording is accurate.
    const auto shown = std::ranges::find_if(plan.due, [](const calendar::DueReminder& d) { return d.notify; });
    TEST_CHECK(shown != plan.due.end() && shown->leadSeconds == 600);
  }

  // ---- reminders coming due at different times still notify separately ----
  {
    const auto now = at(1'700'000'000);
    const CalendarEvent event = timedEvent("a", now + minutes{25}, {600, 1800});
    const auto snapshot = snapshotOf({event});
    const auto first = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    TEST_CHECK(first.due.size() == 1 && first.due.front().notify);

    std::unordered_set<std::string> fired{calendar::reminderKey(event, 1800)};
    const auto second = calendar::planReminders(snapshot, defaultConfig(), fired, std::nullopt, now + minutes{15});
    TEST_CHECK(second.due.size() == 1 && second.due.front().notify);
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
    TEST_CHECK(plan.due.size() == 2);
    TEST_CHECK(notifying == 1);
  }

  // ---- distinct meetings at the same time still notify separately ----
  {
    const auto now = at(1'700'000'000);
    const auto a = timedEvent("a", now + minutes{5}, {600});
    auto b = timedEvent("b", now + minutes{5}, {600});
    b.title = "Other meeting";
    const auto plan = calendar::planReminders(snapshotOf({a, b}), defaultConfig(), {}, std::nullopt, now);
    const auto notifying = std::ranges::count_if(plan.due, [](const calendar::DueReminder& d) { return d.notify; });
    TEST_CHECK(notifying == 2);
  }

  // ---- all-day events never produce per-event reminders ----
  {
    const auto now = at(1'700'000'000);
    CalendarEvent allDay = timedEvent("allday", now + hours{2}, {900});
    allDay.allDay = true;
    const auto plan = calendar::planReminders(snapshotOf({allDay}), defaultConfig(), {}, std::nullopt, now);
    TEST_CHECK(plan.due.empty());
  }

  // ---- nextWake is the earliest future due time ----
  {
    const auto now = at(1'700'000'000);
    const auto snapshot = snapshotOf({
        timedEvent("a", now + hours{2}, {900}),  // due in 1h45m
        timedEvent("b", now + hours{1}, {1800}), // due in 30m
    });
    const auto plan = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    TEST_CHECK(plan.due.empty());
    TEST_CHECK(plan.nextWake.has_value() && *plan.nextWake == now + minutes{30});
  }

  // ---- disabled reminders produce nothing and arm no wakeup ----
  {
    const auto now = at(1'700'000'000);
    auto config = defaultConfig();
    config.enabled = false;
    const auto plan =
        calendar::planReminders(snapshotOf({timedEvent("a", now + minutes{5}, {900})}), config, {}, std::nullopt, now);
    TEST_CHECK(plan.due.empty() && !plan.nextWake.has_value());
  }

  // ---- a declined event never reminds, through its own leads or the default ----
  {
    const auto now = at(1'700'000'000);
    auto withLeads = timedEvent("a", now + minutes{5}, {900});
    withLeads.declined = true;
    auto withDefault = timedEvent("b", now + minutes{5});
    withDefault.declined = true;
    auto config = defaultConfig();
    const auto plan = calendar::planReminders(snapshotOf({withLeads, withDefault}), config, {}, std::nullopt, now);
    TEST_CHECK(plan.due.empty() && !plan.nextWake.has_value());
    config.useEventReminders = false;
    const auto forced = calendar::planReminders(snapshotOf({withLeads, withDefault}), config, {}, std::nullopt, now);
    TEST_CHECK(forced.due.empty());
  }

  // ---- an invalid snapshot is ignored ----
  {
    const auto now = at(1'700'000'000);
    CalendarSnapshot snapshot;
    snapshot.events = {timedEvent("a", now + minutes{5}, {900})};
    snapshot.valid = false;
    const auto plan = calendar::planReminders(snapshot, defaultConfig(), {}, std::nullopt, now);
    TEST_CHECK(plan.due.empty());
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
    TEST_CHECK(today.has_value());
    allDay.start = *today;
    allDay.end = *today + hours{24};

    const auto plan = calendar::planReminders(snapshotOf({allDay}), config, {}, std::nullopt, now);
    TEST_CHECK(plan.digestDue && plan.digest.size() == 1);

    const auto repeat = calendar::planReminders(snapshotOf({allDay}), config, {}, calendar::localDateKey(now), now);
    TEST_CHECK(!repeat.digestDue);
  }

  // ---- digest: a declined all-day event is left out ----
  {
    const auto now = system_clock::now();
    auto config = defaultConfig();
    config.allDayDigestTime = "00:00";
    const auto today = calendar::digestInstantFor(now, "00:00");
    TEST_CHECK(today.has_value());
    CalendarEvent offsite;
    offsite.id = "offsite";
    offsite.title = "Offsite";
    offsite.allDay = true;
    offsite.declined = true;
    offsite.start = *today;
    offsite.end = *today + hours{24};
    const auto plan = calendar::planReminders(snapshotOf({offsite}), config, {}, std::nullopt, now);
    TEST_CHECK(!plan.digestDue && plan.digest.empty());
  }

  // ---- digest: an empty day does not mark the date as handled ----
  {
    const auto now = system_clock::now();
    auto config = defaultConfig();
    config.allDayDigestTime = "00:00";
    const auto plan = calendar::planReminders(snapshotOf({}), config, {}, std::nullopt, now);
    TEST_CHECK(!plan.digestDue);
  }

  // ---- digest: once today's time has passed, tomorrow's digest is armed ----
  {
    const auto now = system_clock::now();
    auto config = defaultConfig();
    config.allDayDigestTime = "00:00";
    const auto tomorrow = time_point_cast<system_clock::duration>(
        current_zone()->to_sys(floor<days>(current_zone()->to_local(now)) + days{1}, choose::earliest)
    );
    const auto shown = calendar::planReminders(snapshotOf({}), config, {}, calendar::localDateKey(now), now);
    TEST_CHECK(shown.nextWake == tomorrow);
    const auto empty = calendar::planReminders(snapshotOf({}), config, {}, std::nullopt, now);
    TEST_CHECK(empty.nextWake == tomorrow);
  }

  // ---- digestInstantFor validates its input ----
  {
    const auto now = system_clock::now();
    TEST_CHECK(!calendar::digestInstantFor(now, "").has_value());
    TEST_CHECK(!calendar::digestInstantFor(now, "9:00").has_value());
    TEST_CHECK(!calendar::digestInstantFor(now, "24:00").has_value());
    TEST_CHECK(calendar::digestInstantFor(now, "09:30").has_value());
  }

  // ---- countdown: rounds up, and every step sits on a whole minute before the event ----
  {
    const auto start = at(1'700'000'000);
    // A reminder fires a hair after its due instant, which is the case that used to render every
    // ten-minute lead as "in 9 min".
    TEST_CHECK(calendar::countdownMinutes(start, start - seconds{600} + milliseconds{7}) == 10);
    TEST_CHECK(calendar::countdownMinutes(start, start - seconds{600}) == 10);
    // The label must change exactly nine minutes before the event, not eight and a half.
    TEST_CHECK(calendar::countdownMinutes(start, start - seconds{541}) == 10);
    TEST_CHECK(calendar::countdownMinutes(start, start - seconds{540}) == 9);
    // And "starting now" belongs to the start itself, not to the last half minute before it.
    TEST_CHECK(calendar::countdownMinutes(start, start - seconds{1}) == 1);
    TEST_CHECK(calendar::countdownMinutes(start, start) == 0);
    TEST_CHECK(calendar::countdownMinutes(start, start + seconds{90}) <= 0);
  }

  // ---- countdown: each wake observes a lower value, and the last one settles ----
  {
    const auto start = at(1'700'000'000);
    auto now = start - seconds{600};
    std::int64_t shown = calendar::countdownMinutes(start, now);
    TEST_CHECK(shown == 10);

    int steps = 0;
    while (auto next = calendar::countdownNextChange(start, now)) {
      TEST_CHECK(*next > now);
      now = *next;
      const std::int64_t updated = calendar::countdownMinutes(start, now);
      TEST_CHECK(updated < shown);
      shown = updated;
      if (++steps > 20) {
        break;
      }
    }
    TEST_CHECK(steps == 10);
    TEST_CHECK(shown <= 0);
    TEST_CHECK(now == start);
  }

  // ---- countdown: a far-out reminder waits instead of ticking all day ----
  {
    const auto start = at(1'700'000'000);
    const auto next = calendar::countdownNextChange(start, start - hours{24});
    TEST_CHECK(next.has_value() && *next == start - calendar::kCountdownWindow);
    TEST_CHECK(!calendar::countdownNextChange(start, start).has_value());
  }

  // ---- all-day coverage: the exclusive end date holds across a short (spring-forward) day ----
  // libstdc++'s current_zone() ignores TZ, so the case uses the host zone's own 2026 transition and
  // only checks the plain date boundaries on hosts without DST.
  {
    const auto* zone = current_zone();
    const auto midnight = [zone](local_days day) {
      return time_point_cast<system_clock::duration>(zone->to_sys(day, choose::earliest));
    };
    // The last day of an all-day event spanning [firstDay, firstDay + 2): prefer a day that is shorter
    // than 24 hours, since subtracting elapsed hours from the exclusive end lands on the wrong date.
    local_days lastDay{2026y / January / 2};
    for (local_days day{2026y / January / 2}; day < local_days{2027y / January / 1}; day += days{1}) {
      if (midnight(day + days{1}) - midnight(day) < hours{24}) {
        lastDay = day;
        break;
      }
    }
    CalendarEvent allDay;
    allDay.id = "dst";
    allDay.allDay = true;
    allDay.start = midnight(lastDay - days{1});
    allDay.end = midnight(lastDay + days{1});
    TEST_CHECK(calendar::allDayEventCoversDate(allDay, midnight(lastDay) + hours{12}));
    TEST_CHECK(!calendar::allDayEventCoversDate(allDay, midnight(lastDay + days{1}) + hours{12}));
  }

  return 0;
}
