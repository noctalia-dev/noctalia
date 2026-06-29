#pragma once

#include "config/config_types.h"
#include "notification.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum class NotificationEvent {
  Added,
  Updated,
  Closed,
};

struct NotificationHistoryEntry {
  Notification notification;
  bool active = true;
  bool seen = false;
  std::optional<CloseReason> closeReason;
  std::uint64_t eventSerial = 0;
};

constexpr int32_t kDefaultNotificationTimeout = 6000;

// Upper bound on action pairs (key + label) kept per notification. Enforced once at ingress
// in addOrReplace(); render sites clamp defensively against the same value.
constexpr std::size_t kMaxNotificationActions = 6;

// Freedesktop expire_timeout: 0 = persistent, -1 = server default, positive = milliseconds.
// Normalize once at Notify ingress so manager timers and toast countdowns stay aligned.
[[nodiscard]] inline int32_t normalizeNotifyExpireTimeout(int32_t expireTimeout) noexcept {
  if (expireTimeout == 0) {
    return 0;
  }
  if (expireTimeout < 0) {
    return kDefaultNotificationTimeout;
  }
  return expireTimeout;
}

struct NotificationRequest {
  uint32_t replacesId = 0;
  std::string appName;
  std::string summary;
  std::string body;
  Urgency urgency = Urgency::Normal;
  int32_t timeout = kDefaultNotificationTimeout;
  NotificationOrigin origin = NotificationOrigin::External;
  bool transient = false;
  std::vector<std::string> actions;
  std::optional<std::string> icon = std::nullopt;
  std::optional<NotificationImageData> imageData = std::nullopt;
  std::optional<std::string> category = std::nullopt;
  std::optional<std::string> desktopEntry = std::nullopt;
  std::optional<uint32_t> forcedId = std::nullopt;
};

class NotificationManager {
public:
  NotificationManager() = default;

  using EventCallback = std::function<void(const Notification&, NotificationEvent)>;
  using ActionInvokeCallback = std::function<void(uint32_t, const std::string&)>;
  using CloseCallback = std::function<void(uint32_t, CloseReason)>;
  using StateCallback = std::function<void()>;

  // Register a callback for notification events. Returns a token for removal.
  int addEventCallback(EventCallback callback);
  void removeEventCallback(int token);

  // Adds a new notification or updates an existing one.
  uint32_t addOrReplace(NotificationRequest request);

  // Adopts a notification id assigned by an external server (e.g. KDE Plasma).
  uint32_t adoptExternal(uint32_t id, NotificationRequest request);

  // Adds an internal notification to the same store as external notifications.
  uint32_t addInternal(
      std::string appName, std::string summary, std::string body, Urgency urgency = Urgency::Normal,
      int32_t timeout = kDefaultNotificationTimeout, std::optional<std::string> icon = std::nullopt,
      std::optional<NotificationImageData> imageData = std::nullopt, std::optional<std::string> category = std::nullopt,
      std::optional<std::string> desktopEntry = std::nullopt
  );

  void setActionInvokeCallback(ActionInvokeCallback callback);
  void setCloseCallback(CloseCallback callback);
  [[nodiscard]] bool hasPendingDBusClose(uint32_t id) const noexcept;
  [[nodiscard]] bool invokeAction(uint32_t id, const std::string& actionKey, bool closeAfterInvoke = true);
  // Emits ActionInvoked with "inline-reply::<text>" (KDE quick-reply convention).
  [[nodiscard]] bool invokeInlineReply(uint32_t id, const std::string& replyText, bool closeAfterInvoke = true);

  // Closes a notification by ID. Returns false if not found.
  bool close(uint32_t id, CloseReason reason = CloseReason::ClosedByCall);

  // Returns IDs of all notifications whose expiry_time has passed.
  [[nodiscard]] std::vector<uint32_t> expiredIds() const;

  // Returns ms until the next expiry, or -1 if none are scheduled.
  [[nodiscard]] int nextExpiryTimeoutMs() const;

  // Closes all expired notifications.
  void processExpired();

  // Pauses the expiry timer on a notification (clears its expiryTime). Used by the
  // toast to freeze the auto-dismiss while the user hovers a card.
  void pauseExpiry(uint32_t id);
  // Resumes expiry by setting expiryTime = now + remainingMs.
  void resumeExpiry(uint32_t id, int32_t remainingMs);

  // All stored notifications.
  [[nodiscard]] const std::deque<Notification>& all() const noexcept;

  // Recent notification history including closed notifications.
  [[nodiscard]] const std::deque<NotificationHistoryEntry>& history() const noexcept;
  [[nodiscard]] std::uint64_t changeSerial() const noexcept;
  void removeHistoryEntry(uint32_t id, std::optional<CloseReason> dbusCloseReason = std::nullopt);
  void clearHistory();
  void setFilters(std::vector<NotificationFilterConfig> filters);
  [[nodiscard]] const std::vector<NotificationFilterConfig>& filters() const noexcept;
  void setDoNotDisturb(bool enabled);
  [[nodiscard]] bool doNotDisturb() const noexcept;
  [[nodiscard]] bool toggleDoNotDisturb();
  void setStateCallback(StateCallback callback);
  void setSoundPlayer(class SoundPlayer* soundPlayer);

  // Bar indicator: true when at least one notification was added since the user last
  // viewed the notification history (control center notifications tab).
  [[nodiscard]] bool hasUnreadNotificationHistory() const noexcept;
  void markNotificationHistorySeen();

  /// Loads persisted history from disk (call once at startup before emitting events).
  void loadPersistedHistory();
  /// Writes pending history to disk immediately (e.g. shutdown).
  void flushPersistedHistory();

private:
  void upsertHistory(const Notification& notification, bool active, std::optional<CloseReason> closeReason);
  void rebuildHistoryIndex();
  void schedulePersistHistory();
  void persistHistoryToDisk();
  void emitPendingDBusClose(uint32_t id, CloseReason reason);
  [[nodiscard]] bool computeHasUnreadNotificationHistory() const noexcept;
  void notifyUnreadStateChangedIfNeeded(bool previousUnreadState);
  struct ExternalNotificationDispatch {
    bool fullySuppress = false;
    bool showToast = true;
    bool saveHistory = true;
    bool playSound = true;
    bool disallowPermanent = false;
  };
  [[nodiscard]] ExternalNotificationDispatch evaluateExternalDispatch(
      Urgency urgency, std::string_view appName, const std::optional<std::string>& category,
      const std::optional<std::string>& desktopEntry, bool transient
  ) const;
  uint32_t suppressExternal(std::string_view appName, Urgency urgency);

  bool m_persistScheduled = false;

  std::deque<Notification> m_notifications;
  std::unordered_map<uint32_t, size_t> m_idToIndex;
  std::unordered_set<uint32_t> m_suppressedIds;
  std::vector<NotificationFilterConfig> m_filters;
  /// Expired notifications with actions: NotificationClosed deferred until dismiss, action, or history removal.
  std::unordered_set<uint32_t> m_pendingDBusClose;
  std::deque<NotificationHistoryEntry> m_history;
  std::unordered_map<uint32_t, size_t> m_historyIndex;
  std::vector<std::pair<int, EventCallback>> m_eventCallbacks;
  ActionInvokeCallback m_actionInvokeCallback;
  CloseCallback m_closeCallback;
  StateCallback m_stateCallback;
  int m_nextCallbackToken{0};
  uint32_t m_nextId{1};
  std::uint64_t m_changeSerial{0};
  bool m_doNotDisturb = false;
  class SoundPlayer* m_soundPlayer = nullptr;
};
