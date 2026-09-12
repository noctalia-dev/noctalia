#include "calendar/google_reminders.h"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <print>
#include <vector>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "google_client_reminders_test: {}", message);
    }
    return condition;
  }

  using Leads = std::vector<std::int32_t>;

} // namespace

int main() {
  bool ok = true;

  // ---- defaultReminders from the events.list response ----
  {
    const auto response = nlohmann::json::parse(R"({
      "defaultReminders": [{"method": "popup", "minutes": 10}, {"method": "email", "minutes": 60}]
    })");
    ok = expect(
             calendar::detail::googleDefaultReminders(response) == Leads{600},
             "email default reminders were not filtered out"
         )
        && ok;
  }
  {
    ok = expect(
             calendar::detail::googleDefaultReminders(nlohmann::json::object()).empty(),
             "a missing defaultReminders array did not yield empty"
         )
        && ok;
  }

  // ---- useDefault falls back to the calendar defaults ----
  {
    const auto item = nlohmann::json::parse(R"({"reminders": {"useDefault": true}})");
    ok = expect(
             calendar::detail::googleEventReminders(item, Leads{600, 1800}) == (Leads{600, 1800}),
             "useDefault did not pick up the calendar defaults"
         )
        && ok;
  }

  // ---- explicit overrides win over useDefault ----
  {
    const auto item = nlohmann::json::parse(R"({
      "reminders": {"useDefault": true, "overrides": [{"method": "popup", "minutes": 5}]}
    })");
    ok = expect(
             calendar::detail::googleEventReminders(item, Leads{600}) == Leads{300},
             "overrides did not win over useDefault"
         )
        && ok;
  }

  // ---- an event explicitly opting out yields nothing ----
  {
    const auto item = nlohmann::json::parse(R"({"reminders": {"useDefault": false, "overrides": []}})");
    ok = expect(
             calendar::detail::googleEventReminders(item, Leads{600}).empty(),
             "an event opting out of reminders inherited the calendar defaults"
         )
        && ok;
  }

  // ---- a missing reminders object yields nothing ----
  {
    ok = expect(
             calendar::detail::googleEventReminders(nlohmann::json::object(), Leads{600}).empty(),
             "an event without a reminders object inherited the calendar defaults"
         )
        && ok;
  }

  // ---- overrides are filtered, converted, sorted, deduplicated and bounded ----
  {
    const auto item = nlohmann::json::parse(R"({"reminders": {"overrides": [
      {"method": "popup", "minutes": 30},
      {"method": "display", "minutes": 5},
      {"method": "email", "minutes": 1},
      {"method": "popup", "minutes": 30}
    ]}})");
    ok = expect(
             calendar::detail::googleEventReminders(item, {}) == (Leads{300, 1800}),
             "overrides were not filtered, sorted and deduplicated"
         )
        && ok;
  }
  {
    const auto item = nlohmann::json::parse(R"({"reminders": {"overrides": [
      {"method": "popup", "minutes": 60000},
      {"method": "popup", "minutes": -5}
    ]}})");
    ok = expect(
             calendar::detail::googleEventReminders(item, {}).empty(), "out of range override minutes were not rejected"
         )
        && ok;
  }
  {
    nlohmann::json overrides = nlohmann::json::array();
    for (int minutes = 1; minutes <= 10; ++minutes) {
      overrides.push_back({{"method", "popup"}, {"minutes", minutes}});
    }
    const nlohmann::json item{{"reminders", {{"overrides", overrides}}}};
    ok = expect(
             calendar::detail::googleEventReminders(item, {}).size() == calendar::kMaxRemindersPerEvent,
             "override count was not capped"
         )
        && ok;
  }

  return ok ? 0 : 1;
}
