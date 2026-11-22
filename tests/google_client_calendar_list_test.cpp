#include "calendar/google_calendar_list.h"
#include "tests/test_check.h"

#include <nlohmann/json.hpp>

int main() {
  TEST_CHECK(
      calendar::detail::googleCalendarListItemSelected(
          nlohmann::json{{"id", "checked@example.com"}, {"selected", true}}
      )
  );
  TEST_CHECK(!calendar::detail::googleCalendarListItemSelected(
      nlohmann::json{{"id", "unchecked@example.com"}, {"selected", false}}
  ));
  // Omitted selected field uses Google's false default.
  TEST_CHECK(!calendar::detail::googleCalendarListItemSelected(nlohmann::json{{"id", "omitted@example.com"}}));
  return 0;
}
