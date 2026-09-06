#pragma once

#include "calendar/calendar_types.h"
#include "config/config_types.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// Pure reminder planning. Everything here is free of I/O, timers, and services so the firing rules
// stay unit-testable; CalendarReminderMonitor is the stateful shell that drives it.
namespace calendar {

  // At most this many reminders survive per event. Bounds both the fired set and a notification burst
  // from a hostile or hand-edited feed.
  inline constexpr std::size_t kMaxRemindersPerEvent = 5;
  // Leads beyond this are dropped as nonsense (and would arm wakeups a month out).
  inline constexpr std::int32_t kMaxLeadSeconds = 30 * 24 * 3600;
  // Recorded in a fired key when the reminder came from the configured default rather than the event
  // itself, so changing default_lead_minutes never re-notifies an already-fired reminder.
  inline constexpr std::int32_t kDefaultLeadSentinel = -1;
  // Reminders shown individually in one pass; the rest collapse into a single summary so a shell
  // started after a long offline period does not spray toasts.
  inline constexpr std::size_t kMaxCatchUpNotifications = 3;
  // How late a reminder may be discovered and still fire once its event has begun. An at-start
  // reminder (TRIGGER:PT0S) comes due exactly at the start, so without this it could never satisfy
  // "the event has not started yet" and would be silently swallowed. It also lets a wakeup delayed
  // by suspend still deliver an alert that is only moments late. It must be at least the poll
  // ceiling, and it bounds how long a fired key must be kept to prevent a repeat.
  inline constexpr std::chrono::seconds kLateGrace{5 * 60};

  // Drops negative and over-long leads, sorts ascending, deduplicates, truncates to
  // kMaxRemindersPerEvent. Applied by every producer (iCal, Google, cache) so downstream code can
  // assume the invariant.
  void normalizeReminderLeads(std::vector<std::int32_t>& leads);

  // Stable identity for one (event instance, lead) pair. Recurring instances share a UID and differ
  // by start, so start is part of the key. The account is deliberately NOT part of it: the same iCal
  // UID subscribed through two accounts collapses to one key and one notification, which is intended.
  [[nodiscard]] std::string reminderKey(const CalendarEvent& event, std::int32_t leadKey);

  // Start instant embedded in a reminder key, or nullopt when the key is malformed.
  [[nodiscard]] std::optional<std::chrono::system_clock::time_point> reminderKeyStart(std::string_view key);

  // Local wall-clock instant of `hhmm` ("HH:MM") on the local date containing `now`. DST-safe: a time
  // inside a spring-forward gap resolves to the earliest valid instant rather than throwing.
  [[nodiscard]] std::optional<std::chrono::system_clock::time_point>
  digestInstantFor(std::chrono::system_clock::time_point now, std::string_view hhmm);

  // Local calendar date of `now` as "YYYY-MM-DD".
  [[nodiscard]] std::string localDateKey(std::chrono::system_clock::time_point now);

  // True when the all-day `event` covers the local date of `now`.
  [[nodiscard]] bool allDayEventCoversDate(const CalendarEvent& event, std::chrono::system_clock::time_point now);

  // How far ahead a reminder body keeps counting down. A VALARM a day out would otherwise rewrite its
  // notification 1440 times over; outside this window the text stays as it read when the reminder fired.
  inline constexpr std::chrono::minutes kCountdownWindow{60};

  // Whole minutes from `now` to `start`, rounded *up*: the label means "starts in at most N minutes".
  // Truncating instead would render a ten-minute lead as "in 9 min" every single time, because a
  // reminder fires a hair after its due instant. Rounding to nearest fixes that but moves every step
  // to the half minute, so "3 min" would appear 3.5 minutes out and disagree with a glance at the
  // clock for half of each minute. Rounding up anchors every step to a whole minute before the event,
  // and reaches zero — "starting now" — exactly at the start rather than 30 seconds early.
  [[nodiscard]] std::int64_t
  countdownMinutes(std::chrono::system_clock::time_point start, std::chrono::system_clock::time_point now);

  // When the countdown label for `start` next changes, or nullopt once it has settled on "starting now"
  // and can never change again. Always strictly after `now`, so a caller polling on it makes progress
  // instead of spinning on the boundary instant.
  [[nodiscard]] std::optional<std::chrono::system_clock::time_point>
  countdownNextChange(std::chrono::system_clock::time_point start, std::chrono::system_clock::time_point now);

  struct DueReminder {
    const CalendarEvent* event = nullptr;
    std::chrono::system_clock::time_point due;
    std::int32_t leadSeconds = 0;
    bool fromDefaultLead = false;
    // False when an earlier reminder for the same event is already being shown in this pass. The
    // caller must still record the key as fired, or it would notify again on the next tick.
    bool notify = true;
    std::string key;
  };

  struct ReminderPlan {
    std::vector<DueReminder> due;             // fire now, ascending by event start
    std::vector<const CalendarEvent*> digest; // today's all-day events, when the digest is due
    bool digestDue = false;
    std::optional<std::chrono::system_clock::time_point> nextWake;
  };

  // A reminder fires when it is due, it has not fired before, and it is still relevant — meaning its
  // event has not started yet, or it came due within kLateGrace. That covers in-session firing,
  // startup catch-up for missed reminders, at-start reminders, and silently dropping reminders for
  // events that began long ago.
  //
  // When several reminders for one event come due in the same pass (a cold start where both a 30 and
  // a 10 minute reminder are already overdue), only the one closest to the event is marked notify;
  // the rest are returned so the caller can record them as fired without emitting a duplicate.
  //
  // All-day events never produce per-event reminders (explicit VALARMs on them are ignored); they
  // are surfaced solely through the morning-of digest.
  [[nodiscard]] ReminderPlan planReminders(
      const CalendarSnapshot& snapshot, const CalendarConfig::Reminders& config,
      const std::unordered_set<std::string>& fired, const std::optional<std::string>& lastDigestDate,
      std::chrono::system_clock::time_point now
  );

  // Erases keys whose event started more than kLateGrace ago: past that point the firing rule can
  // never match again, so the key is dead weight. Keeping the grace window is what stops a just-fired
  // at-start reminder from being forgotten and immediately re-fired. This is what keeps the fired set
  // bounded without a horizon watermark.
  void pruneFiredKeys(std::unordered_set<std::string>& fired, std::chrono::system_clock::time_point now);

} // namespace calendar
