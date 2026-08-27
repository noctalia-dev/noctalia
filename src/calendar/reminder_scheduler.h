#pragma once

#include "calendar/calendar_service.h"
#include "calendar/calendar_types.h"

#include <chrono>
#include <string>
#include <unordered_set>

class ConfigService;
class NotificationManager;

// Raises a desktop notification a configurable number of minutes before each timed
// calendar event starts. Timer-driven like CalendarService (pollTimeoutMs()/tick());
// it owns no accounts or network of its own — it only reads CalendarService::snapshot()
// and posts through NotificationManager. All-day events are skipped; each event
// instance is reminded at most once per session, keyed by its id + start time.
class CalendarReminderScheduler {
public:
  CalendarReminderScheduler(
      ConfigService& configService, CalendarService& calendar, NotificationManager* notifications
  );
  ~CalendarReminderScheduler();

  CalendarReminderScheduler(const CalendarReminderScheduler&) = delete;
  CalendarReminderScheduler& operator=(const CalendarReminderScheduler&) = delete;

  void initialize();

  [[nodiscard]] int pollTimeoutMs() const;
  void tick();

private:
  [[nodiscard]] bool active() const;
  [[nodiscard]] std::chrono::system_clock::time_point fireTimeFor(const CalendarEvent& event) const;
  [[nodiscard]] static std::string reminderKey(const CalendarEvent& event);
  void deliver(const CalendarEvent& event, std::chrono::system_clock::time_point now) const;

  ConfigService& m_configService;
  CalendarService& m_calendar;
  NotificationManager* m_notifications = nullptr;
  CalendarService::ChangeCallbackId m_changeCallbackId = 0;
  std::unordered_set<std::string> m_fired; // reminder keys already delivered this session
};
