#pragma once

#include "calendar/calendar_reminders.h"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace calendar::detail {

  // Google reminder methods that map onto a local toast. "email" is server-side delivery, matching the
  // VALARM ACTION policy in ical_parser.
  [[nodiscard]] inline bool googleReminderMethodIsNotifiable(const nlohmann::json& entry) {
    const std::string method = entry.value("method", std::string{"popup"});
    return method == "popup" || method == "display";
  }

  [[nodiscard]] inline std::vector<std::int32_t> googleReminderMinutes(const nlohmann::json& array) {
    std::vector<std::int32_t> leads;
    if (!array.is_array()) {
      return leads;
    }
    for (const auto& entry : array) {
      if (!entry.is_object() || !googleReminderMethodIsNotifiable(entry)) {
        continue;
      }
      const auto minutes = entry.find("minutes");
      if (minutes == entry.end() || !minutes->is_number_integer()) {
        continue;
      }
      const std::int64_t seconds = minutes->get<std::int64_t>() * 60;
      if (seconds < 0 || seconds > kMaxLeadSeconds) {
        continue;
      }
      leads.push_back(static_cast<std::int32_t>(seconds));
    }
    normalizeReminderLeads(leads);
    return leads;
  }

  // Top-level defaultReminders[] of an events.list response — the calendar's own default reminders.
  [[nodiscard]] inline std::vector<std::int32_t> googleDefaultReminders(const nlohmann::json& listResponse) {
    const auto node = listResponse.find("defaultReminders");
    if (node == listResponse.end()) {
      return {};
    }
    return googleReminderMinutes(*node);
  }

  // Per-event resolution: explicit overrides win; useDefault falls back to the calendar defaults.
  // Anything else yields empty, which downstream means "use the configured default lead".
  [[nodiscard]] inline std::vector<std::int32_t>
  googleEventReminders(const nlohmann::json& item, const std::vector<std::int32_t>& calendarDefaults) {
    const auto reminders = item.find("reminders");
    if (reminders == item.end() || !reminders->is_object()) {
      return {};
    }
    if (const auto overrides = reminders->find("overrides"); overrides != reminders->end()) {
      std::vector<std::int32_t> leads = googleReminderMinutes(*overrides);
      if (!leads.empty()) {
        return leads;
      }
    }
    if (reminders->value("useDefault", false)) {
      return calendarDefaults;
    }
    return {};
  }

} // namespace calendar::detail
