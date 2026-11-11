#include "calendar/google_reminders.h"
#include "tests/test_check.h"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <vector>

namespace {

  using Leads = std::vector<std::int32_t>;

} // namespace

int main() {
  // ---- defaultReminders from the events.list response ----
  {
    const auto response = nlohmann::json::parse(R"({
      "defaultReminders": [{"method": "popup", "minutes": 10}, {"method": "email", "minutes": 60}]
    })");
    TEST_CHECK(calendar::detail::googleDefaultReminders(response) == Leads{600});
  }
  {
    TEST_CHECK(calendar::detail::googleDefaultReminders(nlohmann::json::object()).empty());
  }

  // ---- declined detection reads only the authenticated user's attendee entry ----
  {
    const auto item = nlohmann::json::parse(R"({"attendees": [
      {"email": "organizer@example.com", "organizer": true, "responseStatus": "accepted"},
      {"email": "me@example.com", "self": true, "responseStatus": "declined"}
    ]})");
    TEST_CHECK(calendar::detail::googleEventDeclinedBySelf(item));
  }
  {
    const auto item = nlohmann::json::parse(R"({"attendees": [
      {"email": "other@example.com", "responseStatus": "declined"},
      {"email": "me@example.com", "self": true, "responseStatus": "accepted"}
    ]})");
    TEST_CHECK(!calendar::detail::googleEventDeclinedBySelf(item));
  }
  {
    TEST_CHECK(!calendar::detail::googleEventDeclinedBySelf(nlohmann::json::object()));
  }

  // ---- useDefault falls back to the calendar defaults ----
  {
    const auto item = nlohmann::json::parse(R"({"reminders": {"useDefault": true}})");
    TEST_CHECK(calendar::detail::googleEventReminders(item, Leads{600, 1800}) == (Leads{600, 1800}));
  }

  // ---- useDefault on a calendar without defaults leaves the configured default lead in charge ----
  {
    const auto item = nlohmann::json::parse(R"({"reminders": {"useDefault": true}})");
    TEST_CHECK(!calendar::detail::googleEventReminders(item, {}).has_value());
  }

  // ---- explicit overrides win over useDefault ----
  {
    const auto item = nlohmann::json::parse(R"({
      "reminders": {"useDefault": true, "overrides": [{"method": "popup", "minutes": 5}]}
    })");
    TEST_CHECK(calendar::detail::googleEventReminders(item, Leads{600}) == Leads{300});
  }

  // ---- an event with its notifications removed is explicitly silent ----
  {
    const auto item = nlohmann::json::parse(R"({"reminders": {"useDefault": false, "overrides": []}})");
    TEST_CHECK(calendar::detail::googleEventReminders(item, Leads{600}) == Leads{});
  }
  {
    const auto item = nlohmann::json::parse(R"({"reminders": {"useDefault": false}})");
    TEST_CHECK(calendar::detail::googleEventReminders(item, Leads{600}) == Leads{});
  }

  // ---- a missing reminders object says nothing ----
  {
    TEST_CHECK(!calendar::detail::googleEventReminders(nlohmann::json::object(), Leads{600}).has_value());
  }

  // ---- overrides are filtered, converted, sorted, deduplicated and bounded ----
  {
    const auto item = nlohmann::json::parse(R"({"reminders": {"overrides": [
      {"method": "popup", "minutes": 30},
      {"method": "display", "minutes": 5},
      {"method": "email", "minutes": 1},
      {"method": "popup", "minutes": 30}
    ]}})");
    TEST_CHECK(calendar::detail::googleEventReminders(item, {}) == (Leads{300, 1800}));
  }
  {
    const auto item = nlohmann::json::parse(R"({"reminders": {"overrides": [
      {"method": "popup", "minutes": 60000},
      {"method": "popup", "minutes": -5}
    ]}})");
    TEST_CHECK(!calendar::detail::googleEventReminders(item, {}).has_value());
  }
  {
    nlohmann::json overrides = nlohmann::json::array();
    for (int minutes = 1; minutes <= 10; ++minutes) {
      overrides.push_back({{"method", "popup"}, {"minutes", minutes}});
    }
    const nlohmann::json item{{"reminders", {{"overrides", overrides}}}};
    TEST_CHECK(
        calendar::detail::googleEventReminders(item, {}).value_or(Leads{}).size() == calendar::kMaxRemindersPerEvent
    );
  }

  return 0;
}
