#pragma once

#include "calendar/calendar_reminders.h"
#include "calendar/calendar_types.h"
#include "config/config_types.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>

class ConfigService;
class NotificationManager;

// Most reminders that have fired but whose event has not started yet. Overflow only happens with
// hundreds of events simultaneously inside their reminder window; the degraded behavior is a possible
// repeat after a restart, which beats silently dropping reminders.
inline constexpr std::size_t kMaxPersistedFiredKeys = 256;

// Emits notifications for upcoming calendar events. Level-triggered like BatteryWarningMonitor —
// evaluate() is safe to call on every tick, on snapshot change, and on config reload — but it also
// advertises its own poll deadline, because reminders are time-triggered rather than state-triggered.
//
// Reminders for events carrying a meeting link get a freedesktop "default" action, so clicking the
// toast body opens the link. Snooze is still out of scope; NotificationRequest::actions is the hook.
class CalendarReminderMonitor {
public:
  CalendarReminderMonitor(ConfigService& configService, NotificationManager& notifications);

  // Loads persisted state. Must run before the first onSnapshotChanged so a restart does not
  // re-notify reminders that already fired.
  void initialize();
  void onSnapshotChanged(const CalendarSnapshot& snapshot);
  void onConfigReload();

  [[nodiscard]] int pollTimeoutMs() const;
  void tick();

private:
  void evaluate(std::chrono::system_clock::time_point now);
  // Opens the meeting link of the notification the user clicked. Registered with
  // NotificationManager::setInternalActionCallback.
  void onNotificationAction(std::uint32_t id, const std::string& actionKey, const std::string& activationToken);
  void fireReminder(const calendar::DueReminder& due, std::chrono::system_clock::time_point now);
  void fireCatchUpSummary(std::size_t remaining);
  void fireDigest(std::span<const CalendarEvent* const> events);
  void loadPersistedState();
  void persistState();
  void schedulePersist();
  [[nodiscard]] bool active() const noexcept;

  ConfigService& m_configService;
  NotificationManager& m_notifications;
  // Owned by CalendarService, which outlives this monitor.
  const CalendarSnapshot* m_snapshot = nullptr;
  std::unordered_set<std::string> m_fired;
  std::optional<std::string> m_lastDigestDate;
  std::optional<std::chrono::system_clock::time_point> m_nextWake;
  // Meeting link per outstanding reminder notification, consumed when the user clicks it. Bounded
  // because a reminder that is never clicked would otherwise linger here for the session.
  std::deque<std::pair<std::uint32_t, std::string>> m_actionUrls;
  bool m_initialized = false;
  bool m_persistScheduled = false;
};
