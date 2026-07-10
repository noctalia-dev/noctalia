#include "calendar/google_calendar_list.h"

#include <cstdio>
#include <nlohmann/json.hpp>

namespace {

  bool expectSelected(const nlohmann::json& item, bool expected, const char* message) {
    const bool actual = calendar::detail::googleCalendarListItemSelected(item);
    if (actual != expected) {
      std::fprintf(
          stderr, "google_client_calendar_list_test: %s: expected %s, got %s\n", message,
          expected ? "selected" : "unselected", actual ? "selected" : "unselected"
      );
      return false;
    }
    return true;
  }

} // namespace

int main() {
  bool ok = true;
  ok = expectSelected(
           nlohmann::json{{"id", "checked@example.com"}, {"selected", true}}, true,
           "explicitly selected calendar is synced"
       )
      && ok;
  ok = expectSelected(
           nlohmann::json{{"id", "unchecked@example.com"}, {"selected", false}}, false,
           "explicitly unselected calendar is skipped"
       )
      && ok;
  ok = expectSelected(
           nlohmann::json{{"id", "omitted@example.com"}}, false, "omitted selected field uses Google's false default"
       )
      && ok;
  return ok ? 0 : 1;
}
