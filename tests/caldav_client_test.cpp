#include "calendar/caldav_client.h"
#include "core/deferred_call.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

namespace {

  using namespace std::chrono;

  system_clock::time_point utc(int y, int mo, int d) {
    return sys_days{year{y} / month{static_cast<unsigned>(mo)} / day{static_cast<unsigned>(d)}};
  }

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::fprintf(stderr, "caldav_client_test: %s\n", message);
    }
    return condition;
  }

  bool drainUntil(const std::function<bool()>& predicate) {
    const auto deadline = steady_clock::now() + seconds{15};
    while (steady_clock::now() < deadline) {
      for (auto& callback : DeferredCall::takePending()) {
        callback();
      }
      if (predicate()) {
        return true;
      }
      std::this_thread::sleep_for(milliseconds{5});
    }
    return false;
  }

} // namespace

int main() {
  const std::thread::id mainThread = std::this_thread::get_id();
  bool requestIssued = false;
  bool completed = false;
  bool completionOk = false;
  bool completionOnMainThread = false;
  std::size_t eventCount = 1;

  const std::string body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                           "<D:multistatus xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\">"
                           "<D:response><D:propstat><D:prop><C:calendar-data><![CDATA["
                           "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:counted\r\n"
                           "DTSTART:20200101T000000Z\r\nDTEND:20200101T000001Z\r\n"
                           "RRULE:FREQ=SECONDLY;COUNT=10000000\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n"
                           "]]></C:calendar-data></D:prop></D:propstat></D:response></D:multistatus>";

  calendar::CalDavClient client{[&](HttpRequest, calendar::CalDavClient::ResponseCallback callback) {
    requestIssued = true;
    callback(HttpResponse{.transportOk = true, .status = 207, .body = body});
  }};

  calendar::CalDavAccount account;
  account.url = "https://example.test/calendar";
  account.calendarName = "Test";
  account.color = "#123456";

  const auto beforeFetch = steady_clock::now();
  client.fetchEvents(account, utc(2024, 1, 1), utc(2026, 1, 1), false, [&](bool ok, std::vector<CalendarEvent> events) {
    completionOk = ok;
    completionOnMainThread = std::this_thread::get_id() == mainThread;
    eventCount = events.size();
    completed = true;
  });
  const auto fetchElapsed = steady_clock::now() - beforeFetch;

  bool ok = true;
  ok = expect(requestIssued, "fetch did not issue the HTTP request") && ok;
  ok = expect(fetchElapsed < milliseconds{500}, "fetch blocked on counted recurrence expansion") && ok;
  ok = expect(!completed, "completion was delivered synchronously") && ok;
  ok = expect(drainUntil([&]() { return completed; }), "timed out waiting for deferred completion") && ok;
  ok = expect(completionOk, "valid CalDAV response was rejected") && ok;
  ok = expect(completionOnMainThread, "completion was not delivered on the main thread") && ok;
  ok = expect(eventCount == 0, "out-of-window counted recurrence produced events") && ok;
  return ok ? 0 : 1;
}
