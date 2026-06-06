#include "notification_filter.h"

#include "util/string_utils.h"

#include <unordered_set>

namespace {

  std::string normalizeFilterToken(std::string_view value) { return StringUtils::toLower(StringUtils::trim(value)); }

} // namespace

std::vector<std::string> normalizeNotificationBlacklist(std::vector<std::string> blacklist) {
  std::unordered_set<std::string> seen;
  std::vector<std::string> normalized;
  normalized.reserve(blacklist.size());

  for (auto& raw : blacklist) {
    const std::string token = normalizeFilterToken(raw);
    if (token.empty()) {
      continue;
    }
    if (seen.insert(token).second) {
      normalized.push_back(token);
    }
  }

  return normalized;
}

bool notificationMatchesBlacklist(const std::vector<std::string>& blacklist, const NotificationFilterFields& fields) {
  if (blacklist.empty()) {
    return false;
  }

  const std::string appName = normalizeFilterToken(fields.appName);
  const std::string category = fields.category.has_value() ? normalizeFilterToken(*fields.category) : std::string{};
  const std::string desktopEntry =
      fields.desktopEntry.has_value() ? normalizeFilterToken(*fields.desktopEntry) : std::string{};

  for (const auto& token : blacklist) {
    if (token == appName
        || (!category.empty() && token == category)
        || (!desktopEntry.empty() && token == desktopEntry)) {
      return true;
    }
    if (!token.empty() && !appName.empty() && appName.find(token) != std::string::npos) {
      return true;
    }
  }

  return false;
}

std::unordered_set<Urgency> normalizeAllowedUrgencies(std::vector<std::string> values) {
  std::unordered_set<Urgency> allowed;
  for (auto& raw : values) {
    const std::string token = normalizeFilterToken(raw);
    if (token == "low") {
      allowed.insert(Urgency::Low);
    } else if (token == "normal") {
      allowed.insert(Urgency::Normal);
    } else if (token == "critical") {
      allowed.insert(Urgency::Critical);
    }
  }
  return allowed;
}

bool urgencyIsAllowed(const std::unordered_set<Urgency>& allowed, Urgency urgency) noexcept {
  return allowed.empty() || allowed.contains(urgency);
}
