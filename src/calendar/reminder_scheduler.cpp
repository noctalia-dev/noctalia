#include "calendar/reminder_scheduler.h"

#include "config/config_service.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "notification/notification_manager.h"

#include <algorithm>
#include <string>
#include <unordered_set>

namespace {
  constexpr Logger kLog("calendar-reminder");
} // namespace

CalendarReminderScheduler::CalendarReminderScheduler(
    ConfigService& configService, CalendarService& calendar, NotificationManager* notifications
)
    : m_configService(configService), m_calendar(calendar), m_notifications(notifications) {}

CalendarReminderScheduler::~CalendarReminderScheduler() {
  if (m_changeCallbackId != 0) {
    m_calendar.removeChangeCallback(m_changeCallbackId);
  }
}

void CalendarReminderScheduler::initialize() {
  // A refresh replaces the event set, so drop remembered reminders for instances that
  // are no longer present. This bounds m_fired and lets the poll loop re-evaluate the
  // next deadline on its following iteration.
  m_changeCallbackId = m_calendar.addChangeCallback([this]() {
    if (m_fired.empty()) {
      return;
    }
    std::unordered_set<std::string> live;
    live.reserve(m_calendar.snapshot().events.size());
    for (const CalendarEvent& event : m_calendar.snapshot().events) {
      live.insert(reminderKey(event));
    }
    for (auto it = m_fired.begin(); it != m_fired.end();) {
      it = live.contains(*it) ? std::next(it) : m_fired.erase(it);
    }
  });
}

bool CalendarReminderScheduler::active() const {
  const CalendarConfig& cfg = m_configService.config().calendar;
  return m_notifications != nullptr && cfg.enabled && cfg.remindersEnabled && m_calendar.hasData();
}

std::chrono::system_clock::time_point CalendarReminderScheduler::fireTimeFor(const CalendarEvent& event) const {
  const int lead = std::max<std::int32_t>(0, m_configService.config().calendar.reminderLeadMinutes);
  return event.start - std::chrono::minutes{lead};
}

std::string CalendarReminderScheduler::reminderKey(const CalendarEvent& event) {
  const auto startSeconds = std::chrono::duration_cast<std::chrono::seconds>(event.start.time_since_epoch()).count();
  const std::string& base = event.id.empty() ? event.title : event.id;
  return base + "@" + std::to_string(startSeconds);
}

int CalendarReminderScheduler::pollTimeoutMs() const {
  if (!active()) {
    return -1;
  }
  const auto now = std::chrono::system_clock::now();
  std::int64_t best = -1;
  for (const CalendarEvent& event : m_calendar.snapshot().events) {
    if (event.allDay || now >= event.start || m_fired.contains(reminderKey(event))) {
      continue;
    }
    std::int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(fireTimeFor(event) - now).count();
    ms = std::max<std::int64_t>(0, ms);
    best = best < 0 ? ms : std::min(best, ms);
  }
  if (best < 0) {
    return -1;
  }
  // Cap so the loop re-evaluates periodically (freshly synced events, wall-clock jumps).
  return static_cast<int>(std::min<std::int64_t>(best, 60000));
}

void CalendarReminderScheduler::tick() {
  if (!active()) {
    return;
  }
  const auto now = std::chrono::system_clock::now();
  for (const CalendarEvent& event : m_calendar.snapshot().events) {
    if (event.allDay || now >= event.start) {
      continue;
    }
    const std::string key = reminderKey(event);
    if (m_fired.contains(key) || now < fireTimeFor(event)) {
      continue;
    }
    deliver(event, now);
    m_fired.insert(key);
  }
}

void CalendarReminderScheduler::deliver(const CalendarEvent& event, std::chrono::system_clock::time_point now) const {
  // Round to the nearest minute so "in 10 minutes" reads cleanly regardless of sub-minute drift.
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(event.start - now).count();
  const std::int64_t minutes = (seconds + 30) / 60;

  const std::string summary =
      event.title.empty() ? i18n::tr("notifications.internal.calendar-reminder-untitled") : event.title;
  const std::string body = minutes <= 0 ? i18n::tr("notifications.internal.calendar-reminder-now")
                                        : i18n::tr("notifications.internal.calendar-reminder-body", "minutes", minutes);

  kLog.info("event reminder: '{}' starts in {} min", summary, minutes);
  m_notifications->addInternal(
      i18n::tr("notifications.internal.calendar"), summary, body, Urgency::Normal, kDefaultNotificationTimeout,
      std::string("noctalia-glyph:bell")
  );
}
