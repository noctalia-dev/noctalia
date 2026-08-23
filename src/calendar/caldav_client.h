#pragma once

#include "calendar/calendar_types.h"
#include "net/http_client.h"
#include "security/secure_buffer.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace calendar {

  struct CalDavAccount {
    std::string url;                                        // discovered calendar collection URL
    std::string username;                                   // login
    std::shared_ptr<const security::SecureBuffer> password; // app password
    std::string calendarName;
    std::string color;
  };

  class CalDavClient {
  public:
    using ResponseCallback = HttpClient::ResponseCallback;
    using RequestFunction = std::function<void(HttpRequest, ResponseCallback)>;
    using EventCallback = std::function<void(bool ok, std::vector<CalendarEvent>)>;

    explicit CalDavClient(HttpClient& http);
    explicit CalDavClient(RequestFunction request);
    ~CalDavClient();

    CalDavClient(const CalDavClient&) = delete;
    CalDavClient& operator=(const CalDavClient&) = delete;

    // Query a CalDAV collection for events overlapping [start, end] via a calendar-query REPORT
    // with server-side recurrence expansion. cb is delivered later on the main-loop thread and
    // receives ok=false on any transport, HTTP, or parse failure.
    void fetchEvents(
        const CalDavAccount& account, std::chrono::system_clock::time_point start,
        std::chrono::system_clock::time_point end, bool allowRedirectAuth, EventCallback cb
    );

  private:
    struct State;

    RequestFunction m_request;
    std::shared_ptr<State> m_state;
  };

} // namespace calendar
