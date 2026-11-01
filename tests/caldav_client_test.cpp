#include "calendar/caldav_client.h"
#include "core/deferred_call.h"

#include <chrono>
#include <functional>
#include <optional>
#include <print>
#include <string>
#include <thread>

namespace {

  using namespace std::chrono;

  system_clock::time_point utc(int y, int mo, int d) {
    return sys_days{year{y} / month{static_cast<unsigned>(mo)} / day{static_cast<unsigned>(d)}};
  }

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "caldav_client_test: {}", message);
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
  const std::string responsePrefix = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                                     "<D:multistatus xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\">"
                                     "<D:response><D:propstat><D:prop><C:calendar-data><![CDATA["
                                     "BEGIN:VCALENDAR\r\n";
  const std::string responseSuffix = "END:VCALENDAR\r\n"
                                     "]]></C:calendar-data></D:prop></D:propstat></D:response></D:multistatus>";
  const std::string normalBody = responsePrefix
      + "BEGIN:VEVENT\r\nUID:normal\r\nDTSTART:20240101T000000Z\r\n"
        "DTEND:20240101T000001Z\r\nRRULE:FREQ=DAILY;COUNT=3\r\nEND:VEVENT\r\n"
      + responseSuffix;
  const std::string expensiveBody = responsePrefix
      + "BEGIN:VEVENT\r\nUID:counted\r\nDTSTART:20200101T000000Z\r\n"
        "DTEND:20200101T000001Z\r\nRRULE:FREQ=SECONDLY;COUNT=10000000\r\nEND:VEVENT\r\n"
      + responseSuffix;

  // A resource with an unparseable line and no occurrence inside the window is reported as
  // InvalidCalendar. It must be skipped without dropping the other resources of the collection.
  const std::string mixedBody = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                                "<D:multistatus xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\">"
                                "<D:response><D:propstat><D:prop><C:calendar-data><![CDATA["
                                "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:outside\r\nDTSTART:20240301T170000Z\r\n"
                                "DTEND:20240301T180000Z\r\nRRULE:FREQ=WEEKLY;UNTIL=20240301T225959Z\r\n"
                                "this line is not iCalendar\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n"
                                "]]></C:calendar-data></D:prop></D:propstat></D:response>"
                                "<D:response><D:propstat><D:prop><C:calendar-data><![CDATA["
                                "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:inside\r\nDTSTART:20240110T000000Z\r\n"
                                "DTEND:20240110T010000Z\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n"
                                "]]></C:calendar-data></D:prop></D:propstat></D:response></D:multistatus>";

  // A collection made only of invalid resources must still be reported as a failed refresh.
  const std::string allInvalidBody = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                                     "<D:multistatus xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\">"
                                     "<D:response><D:propstat><D:prop><C:calendar-data><![CDATA["
                                     "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:outside\r\nDTSTART:20240301T170000Z\r\n"
                                     "DTEND:20240301T180000Z\r\nRRULE:FREQ=WEEKLY;UNTIL=20240301T225959Z\r\n"
                                     "this line is not iCalendar\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n"
                                     "]]></C:calendar-data></D:prop></D:propstat></D:response></D:multistatus>";

  calendar::CalDavAccount account;
  account.url = "https://example.test/calendar";
  account.calendarName = "Test";
  account.color = "#123456";

  bool requestIssued = false;
  bool completed = false;
  bool completionOk = false;
  bool completionOnMainThread = false;
  std::size_t eventCount = 0;
  calendar::CalDavClient client{[&](HttpRequest, calendar::CalDavClient::ResponseCallback callback) {
    requestIssued = true;
    callback(HttpResponse{.transportOk = true, .status = 207, .body = normalBody});
  }};

  client.fetchEvents(
      account, utc(2024, 1, 1), utc(2024, 2, 1), false,
      [&](calendar::SyncFailure failure, std::vector<CalendarEvent> events) {
        completionOk = failure == calendar::SyncFailure::None;
        completionOnMainThread = std::this_thread::get_id() == mainThread;
        eventCount = events.size();
        completed = true;
      }
  );

  bool ok = true;
  ok = expect(requestIssued, "fetch did not issue the HTTP request") && ok;
  ok = expect(!completed, "completion was delivered synchronously") && ok;
  ok = expect(drainUntil([&]() { return completed; }), "timed out waiting for deferred completion") && ok;
  ok = expect(completionOk, "valid CalDAV response was rejected") && ok;
  ok = expect(completionOnMainThread, "completion was not delivered on the main thread") && ok;
  ok = expect(eventCount == 3, "valid recurrence expansion produced the wrong event count") && ok;

  bool mixedCompleted = false;
  bool mixedOk = false;
  std::size_t mixedCount = 0;
  calendar::CalDavClient mixedClient{[&](HttpRequest, calendar::CalDavClient::ResponseCallback callback) {
    callback(HttpResponse{.transportOk = true, .status = 207, .body = mixedBody});
  }};
  mixedClient.fetchEvents(
      account, utc(2024, 1, 1), utc(2024, 2, 1), false, [&](calendar::SyncFailure failure, auto events) {
        mixedOk = failure == calendar::SyncFailure::None;
        mixedCount = events.size();
        mixedCompleted = true;
      }
  );
  ok = expect(drainUntil([&]() { return mixedCompleted; }), "timed out waiting for mixed collection") && ok;
  ok = expect(mixedOk, "one invalid resource rejected the whole collection") && ok;
  ok = expect(mixedCount == 1, "valid resource next to an invalid one was not kept") && ok;

  bool allInvalidCompleted = false;
  bool allInvalidOk = true;
  calendar::CalDavClient allInvalidClient{[&](HttpRequest, calendar::CalDavClient::ResponseCallback callback) {
    callback(HttpResponse{.transportOk = true, .status = 207, .body = allInvalidBody});
  }};
  allInvalidClient.fetchEvents(
      account, utc(2024, 1, 1), utc(2024, 2, 1), false, [&](calendar::SyncFailure failure, auto) {
        allInvalidOk = failure == calendar::SyncFailure::None;
        allInvalidCompleted = true;
      }
  );
  ok = expect(drainUntil([&]() { return allInvalidCompleted; }), "timed out waiting for all-invalid collection") && ok;
  ok = expect(!allInvalidOk, "a collection with only invalid resources was reported as refreshed") && ok;

  bool expensiveCompleted = false;
  bool expensiveOk = true;
  calendar::CalDavClient limitedClient{[&](HttpRequest, calendar::CalDavClient::ResponseCallback callback) {
    callback(HttpResponse{.transportOk = true, .status = 207, .body = expensiveBody});
  }};
  const auto beforeLimitedFetch = steady_clock::now();
  limitedClient.fetchEvents(account, utc(2024, 1, 1), utc(2026, 1, 1), false, [&](calendar::SyncFailure failure, auto) {
    expensiveOk = failure == calendar::SyncFailure::None;
    expensiveCompleted = true;
  });
  ok = expect(drainUntil([&]() { return expensiveCompleted; }), "timed out waiting for bounded recurrence rejection")
      && ok;
  ok = expect(steady_clock::now() - beforeLimitedFetch < seconds{2}, "recurrence work limit was not bounded") && ok;
  ok = expect(!expensiveOk, "recurrence exceeding the work limit was accepted") && ok;

  // A failed request reports why.
  const auto failureOf = [&](HttpResponse response) {
    std::optional<calendar::SyncFailure> reported;
    calendar::CalDavClient failingClient{[&](HttpRequest, calendar::CalDavClient::ResponseCallback callback) {
      callback(response);
    }};
    failingClient.fetchEvents(account, utc(2024, 1, 1), utc(2024, 2, 1), false, [&](calendar::SyncFailure f, auto) {
      reported = f;
    });
    (void)drainUntil([&]() { return reported.has_value(); });
    return reported;
  };
  ok = expect(
           failureOf(HttpResponse{.transportOk = true, .status = 401}) == calendar::SyncFailure::Authentication,
           "HTTP 401 was not reported as an authentication failure"
       )
      && ok;
  ok = expect(
           failureOf(HttpResponse{.transportOk = true, .status = 404}) == calendar::SyncFailure::NotFound,
           "HTTP 404 was not reported as not found"
       )
      && ok;
  ok = expect(
           failureOf(HttpResponse{.transportOk = true, .status = 503}) == calendar::SyncFailure::Server,
           "HTTP 503 was not reported as a server failure"
       )
      && ok;
  ok = expect(
           failureOf(HttpResponse{.transportOk = false, .timedOut = true}) == calendar::SyncFailure::Timeout,
           "a timed-out transfer was not reported as a timeout"
       )
      && ok;
  ok = expect(
           failureOf(HttpResponse{.transportOk = false}) == calendar::SyncFailure::Network,
           "a transport failure was not reported as a network failure"
       )
      && ok;
  ok = expect(
           failureOf(HttpResponse{.transportOk = true, .status = 207, .body = ""})
               == calendar::SyncFailure::InvalidResponse,
           "an empty body was not reported as an invalid response"
       )
      && ok;

  const auto beforeDestroy = steady_clock::now();
  {
    calendar::CalDavClient stoppingClient{[&](HttpRequest, calendar::CalDavClient::ResponseCallback callback) {
      callback(HttpResponse{.transportOk = true, .status = 207, .body = expensiveBody});
    }};
    stoppingClient.fetchEvents(account, utc(2024, 1, 1), utc(2026, 1, 1), false, [](calendar::SyncFailure, auto) {});
    std::this_thread::sleep_for(milliseconds{20});
  }
  ok = expect(steady_clock::now() - beforeDestroy < milliseconds{500}, "destruction blocked on recurrence parsing")
      && ok;
  return ok ? 0 : 1;
}
