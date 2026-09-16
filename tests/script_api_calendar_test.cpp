#include "scripting/script_api_context.h"

#include <print>
#include <string>
#include <utility>
#include <vector>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "script_api_calendar_test: {}", message);
    }
    return condition;
  }

  scripting::ScriptCalendarEvent makeEvent(std::string id, double startMs) {
    scripting::ScriptCalendarEvent event;
    event.id = std::move(id);
    event.title = "Standup";
    event.calendarName = "Work";
    event.colorHex = "#3367d6";
    event.url = "https://meet.google.com/abc-defg-hij";
    event.startMs = startMs;
    event.endMs = startMs + 1800000.0;
    return event;
  }

} // namespace

int main() {
  bool ok = true;
  scripting::ScriptApiContext context;

  // Before any sync the snapshot must read as invalid, which is what lets noctalia.calendarEvents()
  // return nil so a plugin can tell "not synced yet" from "no upcoming events".
  ok = expect(!context.calendarValid(), "a fresh context must not report a valid calendar") && ok;
  ok = expect(context.calendarEvents().empty(), "a fresh context must expose no events") && ok;

  std::vector<scripting::ScriptCalendarEvent> events;
  events.push_back(makeEvent("evt-1", 1000.0));
  events.push_back(makeEvent("evt-2", 2000.0));
  context.setCalendarEvents(true, std::move(events));

  ok = expect(context.calendarValid(), "calendar must report valid after a successful sync") && ok;
  const auto stored = context.calendarEvents();
  ok = expect(stored.size() == 2, "both events must survive the round trip") && ok;
  if (stored.size() == 2) {
    ok = expect(stored[0].id == "evt-1", "event id must round trip") && ok;
    ok = expect(stored[0].title == "Standup", "event title must round trip") && ok;
    ok = expect(stored[0].colorHex == "#3367d6", "calendar color must round trip") && ok;
    ok = expect(stored[1].startMs == 2000.0, "event start must round trip") && ok;
    ok = expect(stored[1].endMs == 2000.0 + 1800000.0, "event end must round trip") && ok;
    ok = expect(!stored[0].allDay, "allDay must default to false") && ok;
  }

  // An empty successful sync stays valid: the user simply has nothing scheduled. This is the case
  // a plugin must not confuse with a cold start.
  context.setCalendarEvents(true, {});
  ok = expect(context.calendarValid(), "an empty but successful sync must stay valid") && ok;
  ok = expect(context.calendarEvents().empty(), "an empty sync must clear the events") && ok;

  // Losing every account drops back to invalid rather than reporting an empty calendar.
  context.setCalendarEvents(false, {});
  ok = expect(!context.calendarValid(), "an invalidated calendar must stop reporting valid") && ok;

  return ok ? 0 : 1;
}
