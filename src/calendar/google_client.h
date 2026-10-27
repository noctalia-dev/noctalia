#pragma once

#include "calendar/calendar_types.h"
#include "calendar/sync_failure.h"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

class HttpClient;

namespace calendar {

  // Fetches Google Calendar events via the REST API using a bearer access token. Lists the user's
  // calendars, then queries each for events in [start, end] with server-side recurrence expansion
  // (singleEvents=true). The callback gets SyncFailure::None once the list was fetched (a calendar whose
  // events fail is skipped), and SyncFailure::Authentication on HTTP 401, so the caller can refresh the token
  // and retry.
  class GoogleClient {
  public:
    explicit GoogleClient(HttpClient& http);

    void fetchEvents(
        const std::string& accessToken, std::chrono::system_clock::time_point start,
        std::chrono::system_clock::time_point end,
        std::function<void(SyncFailure failure, std::vector<CalendarEvent>)> cb
    );

  private:
    HttpClient& m_http;
  };

} // namespace calendar
