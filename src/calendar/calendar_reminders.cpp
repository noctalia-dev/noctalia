#include "calendar/calendar_reminders.h"

#include "system/day_night_schedule.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <functional>
#include <ranges>
#include <utility>

namespace calendar {

  namespace {

    using namespace std::chrono;

    std::int64_t toUnix(system_clock::time_point tp) { return duration_cast<seconds>(tp.time_since_epoch()).count(); }

    // Local midnight of the day containing `tp`. Falls back to a UTC-anchored day when the time zone
    // database is unavailable, matching how ical_parser degrades.
    std::optional<local_days> localDayOf(system_clock::time_point tp) {
      try {
        return floor<days>(current_zone()->to_local(tp));
      } catch (const std::exception&) {
        return std::nullopt;
      }
    }

    std::optional<system_clock::time_point> toSystemLenient(local_seconds lt) {
      try {
        // choose::earliest keeps a spring-forward gap time (and a fall-back overlap) from throwing.
        return time_point_cast<system_clock::duration>(current_zone()->to_sys(lt, choose::earliest));
      } catch (const std::exception&) {
        return std::nullopt;
      }
    }

    // Identity for an event instance when the feed supplies no UID.
    std::string fallbackId(const CalendarEvent& event) {
      return std::format("h{:016x}", std::hash<std::string>{}(event.title));
    }

  } // namespace

  void normalizeReminderLeads(std::vector<std::int32_t>& leads) {
    std::erase_if(leads, [](std::int32_t lead) { return lead < 0 || lead > kMaxLeadSeconds; });
    std::ranges::sort(leads);
    const auto dupes = std::ranges::unique(leads);
    leads.erase(dupes.begin(), dupes.end());
    if (leads.size() > kMaxRemindersPerEvent) {
      leads.resize(kMaxRemindersPerEvent);
    }
  }

  std::string reminderKey(const CalendarEvent& event, std::int32_t leadKey) {
    const std::string& id = event.id.empty() ? fallbackId(event) : event.id;
    return std::format("{}|{}|{}", id, toUnix(event.start), leadKey);
  }

  std::optional<system_clock::time_point> reminderKeyStart(std::string_view key) {
    const std::size_t last = key.rfind('|');
    if (last == std::string_view::npos || last == 0) {
      return std::nullopt;
    }
    const std::size_t first = key.rfind('|', last - 1);
    if (first == std::string_view::npos) {
      return std::nullopt;
    }
    const std::string_view digits = key.substr(first + 1, last - first - 1);
    std::int64_t value = 0;
    const auto* end = digits.data() + digits.size();
    const auto result = std::from_chars(digits.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
      return std::nullopt;
    }
    return system_clock::time_point{seconds{value}};
  }

  std::optional<system_clock::time_point> digestInstantFor(system_clock::time_point now, std::string_view hhmm) {
    const auto normalized = day_night_schedule::normalizedClock(hhmm);
    if (!normalized.has_value()) {
      return std::nullopt;
    }
    const int hour = ((*normalized)[0] - '0') * 10 + ((*normalized)[1] - '0');
    const int minute = ((*normalized)[3] - '0') * 10 + ((*normalized)[4] - '0');

    const auto day = localDayOf(now);
    if (!day.has_value()) {
      return std::nullopt;
    }
    return toSystemLenient(local_seconds{*day} + hours{hour} + minutes{minute});
  }

  std::string localDateKey(system_clock::time_point now) {
    const auto day = localDayOf(now);
    const sys_days date = day.has_value() ? sys_days{day->time_since_epoch()} : floor<days>(now);
    return std::format("{:%F}", year_month_day{date});
  }

  bool allDayEventCoversDate(const CalendarEvent& event, system_clock::time_point now) {
    if (!event.allDay) {
      return false;
    }
    const auto today = localDayOf(now);
    const auto startDay = localDayOf(event.start);
    if (!today.has_value() || !startDay.has_value()) {
      return false;
    }
    // DTEND of an all-day event is exclusive, so a one-day event ends the following midnight.
    auto endTime = event.end;
    if (event.end > event.start) {
      endTime -= hours{24};
    }
    const auto endDay = localDayOf(endTime);
    const auto last = endDay.has_value() ? std::max(*startDay, *endDay) : *startDay;
    return *today >= *startDay && *today <= last;
  }

  std::int64_t countdownMinutes(system_clock::time_point start, system_clock::time_point now) {
    const auto remaining = start - now;
    if (remaining <= system_clock::duration::zero()) {
      return 0;
    }
    return duration_cast<minutes>(remaining + minutes{1} - system_clock::duration{1}).count();
  }

  std::optional<system_clock::time_point>
  countdownNextChange(system_clock::time_point start, system_clock::time_point now) {
    const std::int64_t shown = countdownMinutes(start, now);
    if (shown <= 0) {
      return std::nullopt; // already reading "starting now"
    }
    if (shown > kCountdownWindow.count()) {
      return start - kCountdownWindow; // nothing to say until the event enters the counting window
    }
    // Rounding up means the label reads `shown` while more than shown - 1 minutes remain, so it drops
    // the moment exactly that many are left. For the last step that instant is the event start itself.
    // poll() never wakes early, so landing on the boundary always observes a lower value.
    return start - seconds{(shown - 1) * 60};
  }

  ReminderPlan planReminders(
      const CalendarSnapshot& snapshot, const CalendarConfig::Reminders& config,
      const std::unordered_set<std::string>& fired, const std::optional<std::string>& lastDigestDate,
      system_clock::time_point now
  ) {
    ReminderPlan plan;
    if (!config.enabled || !snapshot.valid) {
      return plan;
    }

    const auto defaultLead = seconds{std::max(0, config.defaultLeadMinutes) * 60};
    const auto considerWake = [&plan, now](system_clock::time_point when) {
      if (when <= now) {
        return;
      }
      if (!plan.nextWake.has_value() || when < *plan.nextWake) {
        plan.nextWake = when;
      }
    };

    // Events are sorted by start. The scan reaches back kLateGrace so an event that has just begun is
    // still considered — its at-start reminder is only due at that moment — and forward only as far
    // as the widest lead can reach.
    const auto& events = snapshot.events;
    const auto scanEnd =
        std::ranges::upper_bound(events, now + seconds{kMaxLeadSeconds} + hours{24}, {}, &CalendarEvent::start);
    const auto scanBegin = std::ranges::lower_bound(events, now - kLateGrace, {}, &CalendarEvent::start);

    // A reminder that is due still has to be worth showing: either the event is yet to start, or the
    // reminder only just came due. The second clause is what makes at-start reminders possible.
    const auto relevant = [now](const CalendarEvent& event, system_clock::time_point due) {
      return event.start > now || (now - due) <= kLateGrace;
    };

    for (const CalendarEvent& event : std::ranges::subrange(scanBegin, scanEnd)) {
      // All-day events are surfaced only through the digest.
      if (event.allDay) {
        continue;
      }

      const bool useEventLeads = config.useEventReminders && !event.reminderLeadSeconds.empty();
      if (useEventLeads) {
        for (const std::int32_t lead : event.reminderLeadSeconds) {
          const auto due = event.start - seconds{lead};
          std::string key = reminderKey(event, lead);
          if (fired.contains(key)) {
            continue;
          }
          if (due > now) {
            considerWake(due);
          } else if (relevant(event, due)) {
            plan.due.push_back(
                {.event = &event, .due = due, .leadSeconds = lead, .fromDefaultLead = false, .key = std::move(key)}
            );
          }
        }
        continue;
      }

      const auto due = event.start - defaultLead;
      std::string key = reminderKey(event, kDefaultLeadSentinel);
      if (fired.contains(key)) {
        continue;
      }
      if (due > now) {
        considerWake(due);
      } else if (relevant(event, due)) {
        plan.due.push_back(
            {.event = &event,
             .due = due,
             .leadSeconds = static_cast<std::int32_t>(defaultLead.count()),
             .fromDefaultLead = true,
             .key = std::move(key)}
        );
      }
    }

    // Order by event start, then title so duplicates of one meeting are adjacent, then by the
    // reminder closest to the event first.
    std::ranges::sort(plan.due, [](const DueReminder& a, const DueReminder& b) {
      if (a.event->start != b.event->start) {
        return a.event->start < b.event->start;
      }
      if (a.event->title != b.event->title) {
        return a.event->title < b.event->title;
      }
      return a.leadSeconds < b.leadSeconds;
    });

    // Collapse within this pass: on a cold start every reminder for a meeting can be overdue at once,
    // and two toasts for one meeting is noise. Keep the first (closest to the event, so the wording is
    // accurate) and let the caller record the rest as fired silently.
    //
    // Grouping is by (start, title) rather than by event identity so the same meeting subscribed
    // through two accounts collapses even when the providers disagree on the UID — reminderKey cannot
    // merge those. The cost is that two genuinely different events sharing a title and a start time
    // yield one toast, which is rare and still surfaces the time slot.
    std::optional<std::pair<system_clock::time_point, std::string_view>> shown;
    for (DueReminder& item : plan.due) {
      const std::pair<system_clock::time_point, std::string_view> group{item.event->start, item.event->title};
      if (shown.has_value() && *shown == group) {
        item.notify = false;
        continue;
      }
      shown = group;
    }

    // All-day digest: once per local date, and still fires when the shell starts after the configured
    // time — the events are relevant for the whole day.
    if (const auto digestAt = digestInstantFor(now, config.allDayDigestTime); digestAt.has_value()) {
      const std::string today = localDateKey(now);
      const bool alreadyShown = lastDigestDate.has_value() && *lastDigestDate == today;
      if (!alreadyShown) {
        if (now >= *digestAt) {
          for (const CalendarEvent& event : events) {
            if (allDayEventCoversDate(event, now)) {
              plan.digest.push_back(&event);
            }
          }
          plan.digestDue = !plan.digest.empty();
        } else {
          considerWake(*digestAt);
        }
      }
    }

    return plan;
  }

  void pruneFiredKeys(std::unordered_set<std::string>& fired, system_clock::time_point now) {
    // Retain keys through the grace window: a reminder that fired at its event's start can still be
    // re-planned until the window closes, so forgetting it early would notify twice.
    std::erase_if(fired, [now](const std::string& key) {
      const auto start = reminderKeyStart(key);
      return !start.has_value() || *start <= now - kLateGrace;
    });
  }

} // namespace calendar
