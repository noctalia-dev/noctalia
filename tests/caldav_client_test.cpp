#include "calendar/caldav_client.h"
#include "core/deferred_call.h"
#include "tests/test_check.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace {

  using namespace std::chrono;

  system_clock::time_point utc(int y, int mo, int d) {
    return sys_days{year{y} / month{static_cast<unsigned>(mo)} / day{static_cast<unsigned>(d)}};
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
  calendar::CalDavClient client{[&](const HttpRequest&, const calendar::CalDavClient::ResponseCallback& callback) {
    requestIssued = true;
    callback(HttpResponse{.transportOk = true, .status = 207, .body = normalBody});
  }};

  client.fetchEvents(
      account, utc(2024, 1, 1), utc(2024, 2, 1), false,
      [&](calendar::SyncFailure failure, const std::vector<CalendarEvent>& events) {
        completionOk = failure == calendar::SyncFailure::None;
        completionOnMainThread = std::this_thread::get_id() == mainThread;
        eventCount = events.size();
        completed = true;
      }
  );

  TEST_CHECK(requestIssued);
  TEST_CHECK(!completed);
  TEST_CHECK(drainUntil([&] { return completed; }));
  TEST_CHECK(completionOk);
  TEST_CHECK(completionOnMainThread);
  TEST_CHECK(eventCount == 3);

  bool mixedCompleted = false;
  bool mixedOk = false;
  std::size_t mixedCount = 0;
  calendar::CalDavClient mixedClient{[&](const HttpRequest&, const calendar::CalDavClient::ResponseCallback& callback) {
    callback(HttpResponse{.transportOk = true, .status = 207, .body = mixedBody});
  }};
  mixedClient.fetchEvents(
      account, utc(2024, 1, 1), utc(2024, 2, 1), false, [&](calendar::SyncFailure failure, const auto& events) {
        mixedOk = failure == calendar::SyncFailure::None;
        mixedCount = events.size();
        mixedCompleted = true;
      }
  );
  TEST_CHECK(drainUntil([&] { return mixedCompleted; }));
  TEST_CHECK(mixedOk);
  TEST_CHECK(mixedCount == 1);

  bool allInvalidCompleted = false;
  bool allInvalidOk = true;
  calendar::CalDavClient allInvalidClient{[&](const HttpRequest&,
                                              const calendar::CalDavClient::ResponseCallback& callback) {
    callback(HttpResponse{.transportOk = true, .status = 207, .body = allInvalidBody});
  }};
  allInvalidClient.fetchEvents(
      account, utc(2024, 1, 1), utc(2024, 2, 1), false, [&](calendar::SyncFailure failure, const auto&) {
        allInvalidOk = failure == calendar::SyncFailure::None;
        allInvalidCompleted = true;
      }
  );
  TEST_CHECK(drainUntil([&] { return allInvalidCompleted; }));
  TEST_CHECK(!allInvalidOk);

  bool expensiveCompleted = false;
  bool expensiveOk = true;
  calendar::CalDavClient limitedClient{[&](const HttpRequest&,
                                           const calendar::CalDavClient::ResponseCallback& callback) {
    callback(HttpResponse{.transportOk = true, .status = 207, .body = expensiveBody});
  }};
  const auto beforeLimitedFetch = steady_clock::now();
  limitedClient.fetchEvents(
      account, utc(2024, 1, 1), utc(2026, 1, 1), false, [&](calendar::SyncFailure failure, const auto&) {
        expensiveOk = failure == calendar::SyncFailure::None;
        expensiveCompleted = true;
      }
  );
  TEST_CHECK(drainUntil([&] { return expensiveCompleted; }));
  TEST_CHECK(steady_clock::now() - beforeLimitedFetch < seconds{2});
  TEST_CHECK(!expensiveOk);

  // A failed request reports why.
  const auto failureOf = [&](HttpResponse response) {
    std::optional<calendar::SyncFailure> reported;
    calendar::CalDavClient failingClient{
        [&](const HttpRequest&, const calendar::CalDavClient::ResponseCallback& callback) { callback(response); }
    };
    failingClient.fetchEvents(
        account, utc(2024, 1, 1), utc(2024, 2, 1), false, [&](calendar::SyncFailure f, const auto&) { reported = f; }
    );
    drainUntil([&] { return reported.has_value(); });
    return reported;
  };
  TEST_CHECK(failureOf(HttpResponse{.transportOk = true, .status = 401}) == calendar::SyncFailure::Authentication);
  TEST_CHECK(failureOf(HttpResponse{.transportOk = true, .status = 404}) == calendar::SyncFailure::NotFound);
  TEST_CHECK(failureOf(HttpResponse{.transportOk = true, .status = 503}) == calendar::SyncFailure::Server);
  TEST_CHECK(failureOf(HttpResponse{.transportOk = false, .timedOut = true}) == calendar::SyncFailure::Timeout);
  TEST_CHECK(failureOf(HttpResponse{.transportOk = false}) == calendar::SyncFailure::Network);
  TEST_CHECK(
      failureOf(HttpResponse{.transportOk = true, .status = 207, .body = ""}) == calendar::SyncFailure::InvalidResponse
  );

  const auto beforeDestroy = steady_clock::now();
  {
    calendar::CalDavClient stoppingClient{[&](const HttpRequest&,
                                              const calendar::CalDavClient::ResponseCallback& callback) {
      callback(HttpResponse{.transportOk = true, .status = 207, .body = expensiveBody});
    }};
    stoppingClient.fetchEvents(
        account, utc(2024, 1, 1), utc(2026, 1, 1), false, [](calendar::SyncFailure, const auto&) {}
    );
    std::this_thread::sleep_for(milliseconds{20});
  }
  TEST_CHECK(steady_clock::now() - beforeDestroy < milliseconds{500});
  return 0;
}
