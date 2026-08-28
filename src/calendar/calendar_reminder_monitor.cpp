#include "calendar/calendar_reminder_monitor.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "net/url_open.h"
#include "notification/notification_manager.h"
#include "time/time_format.h"

#include <algorithm>
#include <ctime>
#include <format>
#include <ranges>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("calendar-reminders");

  constexpr std::string_view kStateOwner = "calendar_reminders";
  constexpr std::string_view kFiredKey = "fired";
  constexpr std::string_view kDigestDateKey = "digest_date";

  // poll() timeouts run on CLOCK_MONOTONIC, which does not advance across suspend. Without a ceiling
  // a long sleep would resume that much wall-clock time late, so cap how long we stay parked.
  constexpr int kMaxSleepMs = 5 * 60 * 1000;
  // Outstanding clickable reminders tracked at once. Reminders are infrequent, so this only guards
  // against unbounded growth when they are never clicked.
  constexpr std::size_t kMaxTrackedActionUrls = 32;
  // A reminder must survive the user stepping away from the desk, so it never auto-expires:
  // NotificationManager treats a zero timeout as "no expiry". A notification filter with
  // allow_permanent = false can still downgrade it.
  constexpr std::int32_t kStayUntilDismissed = 0;
  // Titles listed individually in the all-day digest before collapsing into "+N more".
  constexpr std::size_t kMaxDigestTitles = 5;

  std::string formatEventTime(const CalendarEvent& event) {
    const std::time_t raw = std::chrono::system_clock::to_time_t(event.start);
    std::tm local{};
    localtime_r(&raw, &local);
    return formatStrftime("%H:%M", local);
  }

  std::string reminderBody(std::int64_t minutes, const std::string& timeText, const std::string& location) {
    std::string when = minutes <= 0
        ? i18n::tr("notifications.internal.calendar-reminder-now", "time", timeText)
        : i18n::tr("notifications.internal.calendar-reminder-in", "minutes", minutes, "time", timeText);
    if (!location.empty()) {
      when = i18n::tr("notifications.internal.calendar-reminder-with-location", "when", when, "location", location);
    }
    return when;
  }

  std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    for (const auto part : std::views::split(text, '\n')) {
      std::string line(part.begin(), part.end());
      if (!line.empty()) {
        out.push_back(std::move(line));
      }
    }
    return out;
  }

} // namespace

CalendarReminderMonitor::CalendarReminderMonitor(ConfigService& configService, NotificationManager& notifications)
    : m_configService(configService), m_notifications(notifications) {}

void CalendarReminderMonitor::initialize() {
  if (m_initialized) {
    return;
  }
  loadPersistedState();
  m_notifications.setInternalActionCallback(
      [this](std::uint32_t id, const std::string& actionKey, const std::string& activationToken) {
        onNotificationAction(id, actionKey, activationToken);
      }
  );
  m_initialized = true;
}

bool CalendarReminderMonitor::active() const noexcept {
  const CalendarConfig& config = m_configService.config().calendar;
  return config.enabled && config.reminders.enabled;
}

void CalendarReminderMonitor::loadPersistedState() {
  if (const auto fired = m_configService.stateString(kStateOwner, kFiredKey); fired.has_value()) {
    for (std::string& key : splitLines(*fired)) {
      m_fired.insert(std::move(key));
    }
  }
  if (auto digest = m_configService.stateString(kStateOwner, kDigestDateKey); digest.has_value() && !digest->empty()) {
    m_lastDigestDate = std::move(*digest);
  }
  calendar::pruneFiredKeys(m_fired, std::chrono::system_clock::now());
}

void CalendarReminderMonitor::persistState() {
  m_persistScheduled = false;

  std::vector<const std::string*> keys;
  keys.reserve(m_fired.size());
  for (const std::string& key : m_fired) {
    keys.push_back(&key);
  }
  if (keys.size() > kMaxPersistedFiredKeys) {
    // Keep the reminders whose events start soonest — those are the ones a restart could re-fire.
    std::ranges::nth_element(keys, keys.begin() + kMaxPersistedFiredKeys, {}, [](const std::string* key) {
      return calendar::reminderKeyStart(*key).value_or(std::chrono::system_clock::time_point::max());
    });
    kLog.warn("fired reminder set exceeded {} entries; persisting the soonest only", kMaxPersistedFiredKeys);
    keys.resize(kMaxPersistedFiredKeys);
  }

  std::string encoded;
  for (const std::string* key : keys) {
    if (!encoded.empty()) {
      encoded.push_back('\n');
    }
    encoded += *key;
  }
  (void)m_configService.setStateString(kStateOwner, kFiredKey, encoded);
  (void)m_configService.setStateString(kStateOwner, kDigestDateKey, m_lastDigestDate.value_or(std::string{}));
}

void CalendarReminderMonitor::schedulePersist() {
  // StateStore rewrites the whole state.toml per call, so coalesce prune-only churn.
  if (m_persistScheduled) {
    return;
  }
  m_persistScheduled = true;
  DeferredCall::callLater([this]() {
    if (m_persistScheduled) {
      persistState();
    }
  });
}

void CalendarReminderMonitor::onSnapshotChanged(const CalendarSnapshot& snapshot) {
  m_snapshot = &snapshot;
  evaluate(std::chrono::system_clock::now());
}

void CalendarReminderMonitor::onConfigReload() {
  m_nextWake.reset();
  DeferredCall::callLater([this]() { evaluate(std::chrono::system_clock::now()); });
}

int CalendarReminderMonitor::pollTimeoutMs() const {
  if (!active() || !m_nextWake.has_value()) {
    return -1;
  }
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(*m_nextWake - std::chrono::system_clock::now()).count();
  return static_cast<int>(std::clamp<std::int64_t>(remaining, 0, kMaxSleepMs));
}

void CalendarReminderMonitor::tick() { evaluate(std::chrono::system_clock::now()); }

void CalendarReminderMonitor::evaluate(std::chrono::system_clock::time_point now) {
  if (!m_initialized || m_snapshot == nullptr) {
    return;
  }
  if (!active()) {
    m_nextWake.reset();
    return;
  }

  const std::size_t firedBefore = m_fired.size();
  calendar::pruneFiredKeys(m_fired, now);
  bool dirty = m_fired.size() != firedBefore;

  const CalendarConfig::Reminders& config = m_configService.config().calendar.reminders;
  const calendar::ReminderPlan plan = calendar::planReminders(*m_snapshot, config, m_fired, m_lastDigestDate, now);

  // A long offline period can leave many reminders due at once; show the soonest few and collapse
  // the rest so the screen is not buried. Reminders the planner already collapsed into an earlier
  // one for the same event are recorded as fired without notifying.
  std::size_t notified = 0;
  std::size_t suppressed = 0;
  for (const calendar::DueReminder& due : plan.due) {
    if (due.notify) {
      if (notified < calendar::kMaxCatchUpNotifications) {
        fireReminder(due, now);
      } else {
        ++suppressed;
      }
      ++notified;
    }
    m_fired.insert(due.key);
    dirty = true;
  }
  if (suppressed > 0) {
    fireCatchUpSummary(suppressed);
  }

  if (plan.digestDue) {
    fireDigest(plan.digest);
    m_lastDigestDate = calendar::localDateKey(now);
    dirty = true;
  }

  m_nextWake = plan.nextWake;

  if (!plan.due.empty() || plan.digestDue) {
    persistState(); // an actual fire is rare and must survive an immediate restart
  } else if (dirty) {
    schedulePersist();
  }
}

void CalendarReminderMonitor::fireReminder(
    const calendar::DueReminder& due, std::chrono::system_clock::time_point now
) {
  const CalendarEvent& event = *due.event;
  const std::string timeText = formatEventTime(event);

  NotificationRequest request;
  // The app name must match the other calendar notifications so notification filters can target them.
  request.appName = i18n::tr("notifications.internal.calendar");
  request.summary = event.title.empty() ? i18n::tr("notifications.internal.calendar-reminder-untitled") : event.title;
  request.body = reminderBody(calendar::countdownMinutes(event.start, now), timeText, event.location);
  request.urgency = Urgency::Normal; // Urgency::Low is excluded from history unconditionally
  request.timeout = kStayUntilDismissed;
  request.origin = NotificationOrigin::Internal;
  request.dndPolicy = NotificationDndPolicy::Respect;
  request.persistInHistory = true;
  request.icon = std::string("noctalia-glyph:calendar-clock");

  // Events with a resolved meeting link become clickable: the toast maps a left click on its body to
  // the freedesktop "default" action.
  const bool clickable = !event.url.empty();
  if (clickable) {
    request.actions = {"default", i18n::tr("notifications.internal.calendar-reminder-open")};
  }

  const std::uint32_t id = m_notifications.addOrReplace(std::move(request));
  if (clickable && id != 0) {
    m_actionUrls.emplace_back(id, event.url);
    while (m_actionUrls.size() > kMaxTrackedActionUrls) {
      m_actionUrls.pop_front();
    }
  }
}

void CalendarReminderMonitor::onNotificationAction(
    std::uint32_t id, const std::string& actionKey, const std::string& activationToken
) {
  if (actionKey != "default") {
    return;
  }
  const auto it = std::ranges::find(m_actionUrls, id, &std::pair<std::uint32_t, std::string>::first);
  if (it == m_actionUrls.end()) {
    return;
  }
  const std::string url = it->second;
  m_actionUrls.erase(it);
  // resolveEventLink already restricted this to http(s), so it is safe to hand to xdg-open.
  if (!net::openInBrowser(url, activationToken)) {
    kLog.warn("failed to open meeting link for notification #{}", id);
  }
}

void CalendarReminderMonitor::fireCatchUpSummary(std::size_t remaining) {
  NotificationRequest request;
  request.appName = i18n::tr("notifications.internal.calendar");
  request.summary = i18n::tr("notifications.internal.calendar-reminder-catchup-title");
  request.body = i18n::tr("notifications.internal.calendar-reminder-catchup-body", "count", remaining);
  request.urgency = Urgency::Normal;
  request.timeout = kStayUntilDismissed;
  request.origin = NotificationOrigin::Internal;
  request.dndPolicy = NotificationDndPolicy::Respect;
  request.persistInHistory = true;
  request.icon = std::string("noctalia-glyph:calendar-clock");
  (void)m_notifications.addOrReplace(std::move(request));
}

void CalendarReminderMonitor::fireDigest(std::span<const CalendarEvent* const> events) {
  std::string body;
  const std::size_t listed = std::min(events.size(), kMaxDigestTitles);
  for (std::size_t i = 0; i < listed; ++i) {
    if (!body.empty()) {
      body.push_back('\n');
    }
    body += events[i]->title.empty() ? i18n::tr("notifications.internal.calendar-reminder-untitled") : events[i]->title;
  }
  if (events.size() > listed) {
    body.push_back('\n');
    body += i18n::tr("notifications.internal.calendar-digest-more", "count", events.size() - listed);
  }

  NotificationRequest request;
  request.appName = i18n::tr("notifications.internal.calendar");
  request.summary = i18n::tr("notifications.internal.calendar-digest-title");
  request.body = std::move(body);
  request.urgency = Urgency::Normal;
  request.timeout = kStayUntilDismissed;
  request.origin = NotificationOrigin::Internal;
  request.dndPolicy = NotificationDndPolicy::Respect;
  request.persistInHistory = true;
  request.icon = std::string("noctalia-glyph:calendar-event");
  (void)m_notifications.addOrReplace(std::move(request));
}
