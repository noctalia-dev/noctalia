#include "notification/notification_filter.h"

#include "util/string_utils.h"

#include <cctype>
#include <regex>
#include <unordered_set>

namespace {

  std::string normalizeFilterToken(std::string_view value) { return StringUtils::toLower(StringUtils::trim(value)); }

  std::string sanitizedFilterName(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
      if (std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '-' || ch == '_') {
        out.push_back(ch);
      } else if (std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == '.' || ch == '/') {
        if (!out.empty() && out.back() != '-') {
          out.push_back('-');
        }
      }
    }
    while (!out.empty() && out.back() == '-') {
      out.pop_back();
    }
    return out;
  }

  std::string uniqueFilterName(
      std::string base, const std::vector<NotificationFilterConfig>& filters, std::optional<std::size_t> ignoreIndex
  ) {
    base = sanitizedFilterName(base);
    if (base.empty()) {
      base = "filter";
    }

    auto exists = [&](std::string_view candidate) {
      for (std::size_t i = 0; i < filters.size(); ++i) {
        if (ignoreIndex.has_value() && *ignoreIndex == i) {
          continue;
        }
        if (filters[i].name == candidate) {
          return true;
        }
      }
      return false;
    };

    if (!exists(base)) {
      return base;
    }

    for (int suffix = 2; suffix < 1000; ++suffix) {
      const std::string candidate = base + "-" + std::to_string(suffix);
      if (!exists(candidate)) {
        return candidate;
      }
    }
    return base + "-dup";
  }

} // namespace

std::string normalizeNotificationMatchToken(std::string token) { return normalizeFilterToken(token); }

bool notificationMatchesToken(std::string_view token, const NotificationFilterFields& fields) {
  if (token.empty()) {
    return false;
  }

  const std::string normalizedToken = normalizeFilterToken(token);
  if (normalizedToken.empty()) {
    return false;
  }

  const std::string appName = normalizeFilterToken(fields.appName);
  const std::string category = fields.category.has_value() ? normalizeFilterToken(*fields.category) : std::string{};
  const std::string desktopEntry =
      fields.desktopEntry.has_value() ? normalizeFilterToken(*fields.desktopEntry) : std::string{};

  if (normalizedToken == appName
      || (!category.empty() && normalizedToken == category)
      || (!desktopEntry.empty() && normalizedToken == desktopEntry)) {
    return true;
  }
  return !appName.empty() && appName.contains(normalizedToken);
}

std::vector<std::string> normalizeNotificationMatchTokens(std::vector<std::string> tokens) {
  std::unordered_set<std::string> seen;
  std::vector<std::string> normalized;
  normalized.reserve(tokens.size());

  for (auto& raw : tokens) {
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

bool notificationMatchesTokens(const std::vector<std::string>& tokens, const NotificationFilterFields& fields) {
  for (const auto& token : tokens) {
    if (notificationMatchesToken(token, fields)) {
      return true;
    }
  }
  return false;
}

std::vector<NotificationFilterConfig> normalizeNotificationFilters(std::vector<NotificationFilterConfig> filters) {
  for (auto& filter : filters) {
    filter.match = normalizeNotificationMatchToken(std::move(filter.match));
    filter.allowedUrgencies = normalizeFilterAllowedUrgencyStrings(std::move(filter.allowedUrgencies));
  }
  normalizeNotificationFilterNames(filters);
  return filters;
}

ResolvedNotificationFilter resolveNotificationFilter(
    const std::vector<NotificationFilterConfig>& filters, const NotificationFilterFields& fields
) {
  for (const auto& filter : filters) {
    if (!filter.enabled || (filter.match.empty() && filter.matchContent.empty())) {
      continue;
    }
    if (!filter.match.empty() && !notificationMatchesToken(filter.match, fields)) {
      continue;
    }
    if (!filter.matchContent.empty()) {
      try {
        std::regex re(filter.matchContent, std::regex_constants::ECMAScript | std::regex_constants::icase);
        const std::string summaryStr(fields.summary);
        const std::string bodyStr(fields.body);
        if (!std::regex_search(summaryStr, re) && !std::regex_search(bodyStr, re)) {
          continue;
        }
      } catch (const std::regex_error&) {
        // Skip filter on invalid regex
        continue;
      }
    }
    return ResolvedNotificationFilter{
        .showToast = filter.showToast,
        .saveHistory = filter.saveHistory,
        .playSound = filter.playSound,
        .allowPermanent = filter.allowPermanent,
        .overrideDuration = filter.overrideDuration,
        .allowedUrgencies = normalizeAllowedUrgencies(filter.allowedUrgencies),
        .matched = true,
    };
  }
  return {};
}

void normalizeNotificationFilterNames(std::vector<NotificationFilterConfig>& filters) {
  std::vector<NotificationFilterConfig> normalized;
  normalized.reserve(filters.size());
  for (auto& filter : filters) {
    if (filter.name.empty() && !filter.match.empty()) {
      filter.name = sanitizedFilterName(filter.match);
    }
    filter.name = uniqueFilterName(filter.name, normalized, std::nullopt);
    normalized.push_back(std::move(filter));
  }
  filters = std::move(normalized);
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

std::vector<std::string> normalizeFilterAllowedUrgencyStrings(std::vector<std::string> values) {
  const auto allowed = normalizeAllowedUrgencies(std::move(values));
  if (allowed.empty() || allowed.size() == 3) {
    return {};
  }
  std::vector<std::string> out;
  if (allowed.contains(Urgency::Low)) {
    out.emplace_back("low");
  }
  if (allowed.contains(Urgency::Normal)) {
    out.emplace_back("normal");
  }
  if (allowed.contains(Urgency::Critical)) {
    out.emplace_back("critical");
  }
  return out;
}

bool urgencyIsAllowed(const std::unordered_set<Urgency>& allowed, Urgency urgency) noexcept {
  return allowed.empty() || allowed.contains(urgency);
}
