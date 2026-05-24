#include "notification_manager.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "notification/notification_history_store.h"
#include "pipewire/sound_player.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <filesystem>
#include <string_view>

namespace {

  constexpr Logger kLog("notification");
  constexpr auto kImplicitDuplicateWindow = std::chrono::seconds(1);

  constexpr std::string_view urgencyStr(Urgency u) noexcept {
    switch (u) {
    case Urgency::Low:
      return "low";
    case Urgency::Normal:
      return "normal";
    case Urgency::Critical:
      return "critical";
    }
    return "unknown";
  }

  constexpr std::string_view kInlineReplyAction = "inline-reply";
  constexpr std::string_view kInlineReplyActionPrefix = "inline-reply::";
  constexpr std::size_t kMaxActionKeyLength = 1024;

  bool notificationHasAction(const Notification& notification, std::string_view actionKey) {
    for (std::size_t i = 0; i + 1 < notification.actions.size(); i += 2) {
      if (notification.actions[i] == actionKey) {
        return true;
      }
    }
    return false;
  }

  constexpr std::string_view originStr(NotificationOrigin o) noexcept {
    switch (o) {
    case NotificationOrigin::External:
      return "external";
    case NotificationOrigin::Internal:
      return "internal";
    }
    return "unknown";
  }

  std::optional<TimePoint> scheduleExpiry(TimePoint now, int32_t timeoutMs) noexcept {
    if (timeoutMs > 0) {
      return now + std::chrono::milliseconds(timeoutMs);
    }
    return std::nullopt; // 0 = persistent (non-positive after normalizeNotifyExpireTimeout)
  }

  std::optional<WallTimePoint> scheduleExpiryWall(WallTimePoint wallNow, int32_t timeoutMs) noexcept {
    if (timeoutMs > 0) {
      return wallNow + std::chrono::milliseconds(timeoutMs);
    }
    return std::nullopt;
  }

  bool hasSameContent(
      const Notification& notification, NotificationOrigin origin, const std::string& appName,
      const std::string& summary, const std::string& body
  ) {
    return notification.origin == origin && notification.appName == appName && notification.summary == summary &&
           notification.body == body;
  }

  bool shouldTrackHistory(NotificationOrigin origin, Urgency urgency) noexcept {
    return origin == NotificationOrigin::External && urgency != Urgency::Low;
  }

  bool shouldRetainHistoryEntry(const NotificationHistoryEntry& entry) noexcept {
    return shouldTrackHistory(entry.notification.origin, entry.notification.urgency) &&
           entry.closeReason != CloseReason::Dismissed;
  }

  bool notificationHasInvokableActions(const Notification& notification) {
    for (std::size_t i = 0; i + 1 < notification.actions.size(); i += 2) {
      if (!notification.actions[i].empty()) {
        return true;
      }
    }
    return false;
  }

} // namespace

void NotificationManager::rebuildHistoryIndex() {
  m_historyIndex.clear();
  for (size_t i = 0; i < m_history.size(); ++i) {
    m_historyIndex[m_history[i].notification.id] = i;
  }
}

void NotificationManager::upsertHistory(
    const Notification& notification, bool active, std::optional<CloseReason> closeReason
) {
  bool seen = false;
  if (const auto it = m_historyIndex.find(notification.id); it != m_historyIndex.end()) {
    seen = m_history[it->second].seen;
    m_history.erase(m_history.begin() + static_cast<std::ptrdiff_t>(it->second));
  }

  m_history.push_back(
      NotificationHistoryEntry{
          .notification = notification,
          .active = active,
          .seen = seen,
          .closeReason = closeReason,
          .eventSerial = ++m_changeSerial,
      }
  );

  constexpr std::size_t kMaxHistoryEntries = 100;
  while (m_history.size() > kMaxHistoryEntries) {
    const NotificationHistoryEntry evicted = m_history.front();
    emitPendingDBusClose(evicted.notification.id, evicted.closeReason.value_or(CloseReason::Expired));
    m_history.pop_front();
  }

  rebuildHistoryIndex();
  schedulePersistHistory();
}

int NotificationManager::addEventCallback(EventCallback callback) {
  int token = m_nextCallbackToken++;
  m_eventCallbacks.emplace_back(token, std::move(callback));
  return token;
}

void NotificationManager::removeEventCallback(int token) {
  std::erase_if(m_eventCallbacks, [token](const auto& pair) { return pair.first == token; });
}

bool NotificationManager::computeHasUnreadNotificationHistory() const noexcept {
  return std::any_of(m_history.begin(), m_history.end(), [](const NotificationHistoryEntry& entry) {
    return !entry.seen;
  });
}

void NotificationManager::notifyUnreadStateChangedIfNeeded(bool previousUnreadState) {
  if (m_stateCallback == nullptr) {
    return;
  }
  if (computeHasUnreadNotificationHistory() != previousUnreadState) {
    m_stateCallback();
  }
}

uint32_t NotificationManager::addOrReplace(
    uint32_t replacesId, std::string appName, std::string summary, std::string body, Urgency urgency, int32_t timeout,
    NotificationOrigin origin, std::vector<std::string> actions, std::optional<std::string> icon,
    std::optional<NotificationImageData> imageData, std::optional<std::string> category,
    std::optional<std::string> desktopEntry
) {
  const auto now = Clock::now();
  const auto wallNow = WallClock::now();

  // Never log summary/body — they may contain sensitive user content (e.g. message previews).
  auto logNotification = [](const Notification& n, std::string_view action) {
    kLog.debug(
        "notification {} #{} origin={} from=\"{}\" urgency={} timeout={}ms", action, n.id, originStr(n.origin),
        n.appName, urgencyStr(n.urgency), n.timeout
    );
  };

  if (replacesId != 0) {
    if (const auto it = m_idToIndex.find(replacesId); it != m_idToIndex.end()) {
      auto& n = m_notifications[it->second];

      // Check if anything changed to avoid duplicate events
      const bool changed =
          (n.appName != appName || n.summary != summary || n.body != body || n.timeout != timeout ||
           n.urgency != urgency || n.origin != origin || n.actions != actions || n.icon != icon ||
           n.imageData != imageData || n.category != category || n.desktopEntry != desktopEntry);

      n.origin = origin;
      n.appName = std::move(appName);
      n.summary = std::move(summary);
      n.body = std::move(body);
      n.timeout = timeout;
      n.urgency = urgency;
      n.actions = std::move(actions);
      n.icon = std::move(icon);
      n.imageData = std::move(imageData);
      n.category = std::move(category);
      n.desktopEntry = std::move(desktopEntry);
      n.receivedTime = now;
      n.expiryTime = scheduleExpiry(now, timeout);
      n.receivedWallClock = wallNow;
      n.expiryWallClock = scheduleExpiryWall(wallNow, timeout);

      logNotification(n, "updated");
      if (shouldTrackHistory(n.origin, n.urgency)) {
        const bool hadUnreadBefore = computeHasUnreadNotificationHistory();
        upsertHistory(n, true, std::nullopt);
        notifyUnreadStateChangedIfNeeded(hadUnreadBefore);
      } else {
        removeHistoryEntry(n.id);
      }

      if (changed) {
        for (auto& [token, cb] : m_eventCallbacks) {
          cb(n, NotificationEvent::Updated);
        }
      }

      return n.id;
    }
  }

  // Suppress immediate duplicate bursts. Later same-content notifications should still be visible.
  for (auto it = m_notifications.rbegin(); it != m_notifications.rend(); ++it) {
    const auto& existing = *it;
    if (hasSameContent(existing, origin, appName, summary, body) &&
        now - existing.receivedTime < kImplicitDuplicateWindow) {
      logNotification(existing, "duplicate ignored");
      return existing.id;
    }
  }

  const uint32_t id = m_nextId++;
  m_notifications.push_back(
      Notification{
          .id = id,
          .origin = origin,
          .appName = std::move(appName),
          .summary = std::move(summary),
          .body = std::move(body),
          .timeout = timeout,
          .urgency = urgency,
          .actions = std::move(actions),
          .icon = std::move(icon),
          .imageData = std::move(imageData),
          .category = std::move(category),
          .desktopEntry = std::move(desktopEntry),
          .receivedTime = now,
          .expiryTime = scheduleExpiry(now, timeout),
          .receivedWallClock = wallNow,
          .expiryWallClock = scheduleExpiryWall(wallNow, timeout),
      }
  );
  m_idToIndex.emplace(id, m_notifications.size() - 1);

  const auto& n = m_notifications.back();
  logNotification(n, "added");
  if (shouldTrackHistory(n.origin, n.urgency)) {
    const bool hadUnreadBefore = computeHasUnreadNotificationHistory();
    upsertHistory(n, true, std::nullopt);
    notifyUnreadStateChangedIfNeeded(hadUnreadBefore);
  }

  for (auto& [token, cb] : m_eventCallbacks) {
    cb(n, NotificationEvent::Added);
  }
  if (!m_doNotDisturb && m_soundPlayer != nullptr) {
    m_soundPlayer->play("notification");
  }

  return n.id;
}

uint32_t NotificationManager::addInternal(
    std::string appName, std::string summary, std::string body, Urgency urgency, int32_t timeout,
    std::optional<std::string> icon, std::optional<NotificationImageData> imageData,
    std::optional<std::string> category, std::optional<std::string> desktopEntry
) {
  return addOrReplace(
      0, std::move(appName), std::move(summary), std::move(body), urgency, timeout, NotificationOrigin::Internal, {},
      std::move(icon), std::move(imageData), std::move(category), std::move(desktopEntry)
  );
}

void NotificationManager::setActionInvokeCallback(ActionInvokeCallback callback) {
  m_actionInvokeCallback = std::move(callback);
}

void NotificationManager::setCloseCallback(CloseCallback callback) { m_closeCallback = std::move(callback); }

bool NotificationManager::hasPendingDBusClose(uint32_t id) const noexcept { return m_pendingDBusClose.contains(id); }

bool NotificationManager::invokeAction(uint32_t id, const std::string& actionKey, bool closeAfterInvoke) {
  if (actionKey.empty()) {
    return false;
  }

  const Notification* notification = nullptr;
  if (const auto it = m_idToIndex.find(id); it != m_idToIndex.end()) {
    notification = &m_notifications[it->second];
  } else if (const auto histIt = m_historyIndex.find(id); histIt != m_historyIndex.end()) {
    if (!hasPendingDBusClose(id)) {
      return false;
    }
    notification = &m_history[histIt->second].notification;
  } else {
    return false;
  }

  if (actionKey == kInlineReplyAction) {
    // This server delivers reply text via invokeInlineReply() as "inline-reply::<text>".
    return false;
  }
  const bool inlineReplyWithPayload = actionKey.starts_with(std::string(kInlineReplyActionPrefix));
  if (inlineReplyWithPayload) {
    if (!notificationHasAction(*notification, kInlineReplyAction)) {
      return false;
    }
  } else if (!notificationHasAction(*notification, actionKey)) {
    return false;
  }

  if (m_actionInvokeCallback) {
    m_actionInvokeCallback(id, actionKey);
  }

  if (closeAfterInvoke) {
    if (m_idToIndex.contains(id)) {
      (void)close(id, CloseReason::Dismissed);
    } else if (const auto histIt = m_historyIndex.find(id); histIt != m_historyIndex.end()) {
      emitPendingDBusClose(id, CloseReason::Dismissed);
      m_history[histIt->second].notification.actions.clear();
      m_history[histIt->second].active = false;
      ++m_changeSerial;
      schedulePersistHistory();
    }
  }
  return true;
}

bool NotificationManager::invokeInlineReply(uint32_t id, const std::string& replyText, bool closeAfterInvoke) {
  if (StringUtils::isBlank(replyText)) {
    return false;
  }

  std::string actionKey;
  actionKey.reserve(kInlineReplyActionPrefix.size() + replyText.size());
  actionKey.append(kInlineReplyActionPrefix);
  actionKey.append(StringUtils::truncateUtf8(replyText, kMaxActionKeyLength - kInlineReplyActionPrefix.size()));
  return invokeAction(id, actionKey, closeAfterInvoke);
}

void NotificationManager::emitPendingDBusClose(uint32_t id, CloseReason reason) {
  if (!m_pendingDBusClose.contains(id)) {
    return;
  }
  m_pendingDBusClose.erase(id);
  if (m_closeCallback) {
    m_closeCallback(id, reason);
  }
}

bool NotificationManager::close(uint32_t id, CloseReason reason) {
  const auto it = m_idToIndex.find(id);
  if (it == m_idToIndex.end()) {
    if (m_pendingDBusClose.contains(id)) {
      emitPendingDBusClose(id, reason);
      removeHistoryEntry(id);
      return true;
    }
    return false;
  }

  const size_t index = it->second;
  const Notification closed = m_notifications[index];
  const bool hadUnreadBefore = computeHasUnreadNotificationHistory();
  const bool historyHandledUnreadChange =
      shouldTrackHistory(closed.origin, closed.urgency) && reason == CloseReason::Dismissed;
  const char* reasonStr = (reason == CloseReason::Expired)     ? "expired"
                          : (reason == CloseReason::Dismissed) ? "dismissed"
                                                               : "closed";
  kLog.debug("notification {} #{}", reasonStr, id);
  if (shouldTrackHistory(closed.origin, closed.urgency)) {
    if (reason == CloseReason::Dismissed) {
      removeHistoryEntry(id, reason);
    } else {
      upsertHistory(closed, false, reason);
    }
  }

  m_notifications.erase(m_notifications.begin() + static_cast<std::ptrdiff_t>(index));
  m_idToIndex.erase(it);

  for (size_t i = index; i < m_notifications.size(); ++i) {
    m_idToIndex[m_notifications[i].id] = i;
  }

  for (auto& [token, cb] : m_eventCallbacks) {
    cb(closed, NotificationEvent::Closed);
  }

  const bool deferDBusClose = reason == CloseReason::Expired && notificationHasInvokableActions(closed);
  if (deferDBusClose) {
    m_pendingDBusClose.insert(id);
  } else if (m_closeCallback) {
    m_closeCallback(id, reason);
  }

  if (!historyHandledUnreadChange) {
    notifyUnreadStateChangedIfNeeded(hadUnreadBefore);
  }

  return true;
}

const std::deque<Notification>& NotificationManager::all() const noexcept { return m_notifications; }

const std::deque<NotificationHistoryEntry>& NotificationManager::history() const noexcept { return m_history; }

std::uint64_t NotificationManager::changeSerial() const noexcept { return m_changeSerial; }

void NotificationManager::removeHistoryEntry(uint32_t id, std::optional<CloseReason> dbusCloseReason) {
  const auto it = m_historyIndex.find(id);
  if (it == m_historyIndex.end()) {
    return;
  }

  const bool hadUnreadBefore = computeHasUnreadNotificationHistory();
  const CloseReason reason =
      dbusCloseReason.value_or(m_history[it->second].closeReason.value_or(CloseReason::Dismissed));
  emitPendingDBusClose(id, reason);
  m_history.erase(m_history.begin() + static_cast<std::ptrdiff_t>(it->second));
  ++m_changeSerial;
  rebuildHistoryIndex();
  schedulePersistHistory();
  notifyUnreadStateChangedIfNeeded(hadUnreadBefore);
}

void NotificationManager::clearHistory() {
  if (m_history.empty()) {
    return;
  }

  for (const auto& entry : m_history) {
    emitPendingDBusClose(entry.notification.id, entry.closeReason.value_or(CloseReason::Expired));
  }

  m_history.clear();
  m_historyIndex.clear();
  ++m_changeSerial;
  markNotificationHistorySeen();
  schedulePersistHistory();
}

std::vector<uint32_t> NotificationManager::expiredIds() const {
  const auto now = Clock::now();
  std::vector<uint32_t> ids;
  for (const auto& n : m_notifications) {
    if (n.expiryTime && now >= *n.expiryTime) {
      ids.push_back(n.id);
    }
  }
  return ids;
}

int NotificationManager::nextExpiryTimeoutMs() const {
  int expiryMs = -1;
  const auto now = Clock::now();
  for (const auto& n : m_notifications) {
    if (n.expiryTime) {
      const auto ms = std::chrono::ceil<std::chrono::milliseconds>(*n.expiryTime - now).count();
      const int clamped = static_cast<int>(std::max<long long>(0, ms));
      if (expiryMs < 0 || clamped < expiryMs) {
        expiryMs = clamped;
      }
    }
  }
  return expiryMs;
}

void NotificationManager::processExpired() {
  for (const uint32_t id : expiredIds()) {
    close(id, CloseReason::Expired);
  }
}

void NotificationManager::pauseExpiry(uint32_t id) {
  const auto it = m_idToIndex.find(id);
  if (it == m_idToIndex.end()) {
    return;
  }
  m_notifications[it->second].expiryTime.reset();
  m_notifications[it->second].expiryWallClock.reset();
}

void NotificationManager::resumeExpiry(uint32_t id, int32_t remainingMs) {
  const auto it = m_idToIndex.find(id);
  if (it == m_idToIndex.end()) {
    return;
  }
  if (remainingMs <= 0) {
    m_notifications[it->second].expiryTime = Clock::now();
    m_notifications[it->second].expiryWallClock = WallClock::now();
    return;
  }
  const auto steadyResume = Clock::now();
  const auto wallResume = WallClock::now();
  m_notifications[it->second].expiryTime = steadyResume + std::chrono::milliseconds(remainingMs);
  m_notifications[it->second].expiryWallClock = wallResume + std::chrono::milliseconds(remainingMs);
}

void NotificationManager::setDoNotDisturb(bool enabled) {
  if (m_doNotDisturb == enabled) {
    return;
  }
  m_doNotDisturb = enabled;
  if (m_stateCallback) {
    m_stateCallback();
  }
}

bool NotificationManager::doNotDisturb() const noexcept { return m_doNotDisturb; }

bool NotificationManager::toggleDoNotDisturb() {
  setDoNotDisturb(!m_doNotDisturb);
  return doNotDisturb();
}

void NotificationManager::setStateCallback(StateCallback callback) { m_stateCallback = std::move(callback); }

void NotificationManager::setSoundPlayer(SoundPlayer* soundPlayer) { m_soundPlayer = soundPlayer; }

bool NotificationManager::hasUnreadNotificationHistory() const noexcept {
  return computeHasUnreadNotificationHistory();
}

void NotificationManager::markNotificationHistorySeen() {
  const bool hadUnreadBefore = computeHasUnreadNotificationHistory();
  if (!hadUnreadBefore) {
    return;
  }
  for (auto& entry : m_history) {
    entry.seen = true;
  }
  schedulePersistHistory();
  notifyUnreadStateChangedIfNeeded(hadUnreadBefore);
}

void NotificationManager::schedulePersistHistory() {
  if (m_persistScheduled) {
    return;
  }
  m_persistScheduled = true;
  DeferredCall::callLater([this]() {
    m_persistScheduled = false;
    persistHistoryToDisk();
  });
}

void NotificationManager::persistHistoryToDisk() {
  const std::string dir = FileUtils::stateDir();
  if (dir.empty()) {
    return;
  }
  std::filesystem::path path(dir);
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  path /= "notification_history.json";
  (void)saveNotificationHistoryToFile(path, m_history, m_nextId, m_changeSerial);
}

void NotificationManager::loadPersistedHistory() {
  const std::string dir = FileUtils::stateDir();
  if (dir.empty()) {
    return;
  }
  std::filesystem::path path(dir);
  path /= "notification_history.json";
  std::uint32_t nextId = m_nextId;
  std::uint64_t serial = m_changeSerial;
  std::deque<NotificationHistoryEntry> loaded;
  if (!loadNotificationHistoryFromFile(path, loaded, nextId, serial)) {
    return;
  }
  std::erase_if(loaded, [](const NotificationHistoryEntry& entry) { return !shouldRetainHistoryEntry(entry); });
  m_history = std::move(loaded);
  m_nextId = nextId;
  m_changeSerial = serial;
  rebuildHistoryIndex();
}

void NotificationManager::flushPersistedHistory() {
  const std::vector<uint32_t> pending(m_pendingDBusClose.begin(), m_pendingDBusClose.end());
  for (const uint32_t id : pending) {
    emitPendingDBusClose(id, CloseReason::Expired);
  }
  persistHistoryToDisk();
}
