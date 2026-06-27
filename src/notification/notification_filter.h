#pragma once

#include "config/config_types.h"
#include "notification.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

struct NotificationFilterFields {
  std::string_view appName;
  std::optional<std::string_view> category;
  std::optional<std::string_view> desktopEntry;
};

struct ResolvedNotificationFilter {
  bool showToast = true;
  bool saveHistory = true;
  bool playSound = true;
  bool allowPermanent = true;
  /// Empty = all urgencies allowed for this filter.
  std::unordered_set<Urgency> allowedUrgencies;
  bool matched = false;
};

[[nodiscard]] std::string normalizeNotificationMatchToken(std::string token);

[[nodiscard]] bool notificationMatchesToken(std::string_view token, const NotificationFilterFields& fields);

[[nodiscard]] std::vector<std::string> normalizeNotificationMatchTokens(std::vector<std::string> tokens);

[[nodiscard]] bool
notificationMatchesTokens(const std::vector<std::string>& tokens, const NotificationFilterFields& fields);

[[nodiscard]] std::vector<NotificationFilterConfig>
normalizeNotificationFilters(std::vector<NotificationFilterConfig> filters);

[[nodiscard]] ResolvedNotificationFilter
resolveNotificationFilter(const std::vector<NotificationFilterConfig>& filters, const NotificationFilterFields& fields);

void normalizeNotificationFilterNames(std::vector<NotificationFilterConfig>& filters);

/// Empty result means all urgencies are allowed.
[[nodiscard]] std::unordered_set<Urgency> normalizeAllowedUrgencies(std::vector<std::string> values);

/// Canonical config form: empty vector when all three levels are allowed.
[[nodiscard]] std::vector<std::string> normalizeFilterAllowedUrgencyStrings(std::vector<std::string> values);

[[nodiscard]] bool urgencyIsAllowed(const std::unordered_set<Urgency>& allowed, Urgency urgency) noexcept;

// Deprecated aliases kept for existing call sites/tests.
[[nodiscard]] inline std::vector<std::string> normalizeNotificationBlacklist(std::vector<std::string> blacklist) {
  return normalizeNotificationMatchTokens(std::move(blacklist));
}

[[nodiscard]] inline bool
notificationMatchesBlacklist(const std::vector<std::string>& blacklist, const NotificationFilterFields& fields) {
  return notificationMatchesTokens(blacklist, fields);
}
