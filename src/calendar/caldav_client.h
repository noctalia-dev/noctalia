#pragma once

#include "calendar/calendar_types.h"
#include "security/secure_buffer.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class HttpClient;

namespace calendar {

  struct CalDavAccount {
    std::string url;                                        // discovered calendar collection URL
    std::string username;                                   // login
    std::shared_ptr<const security::SecureBuffer> password; // app password
    std::string calendarName;
    std::string color;
  };

  // Query a CalDAV collection for events overlapping [start, end] via a calendar-query REPORT with
  // server-side recurrence expansion. cb receives ok=false on any transport/HTTP/parse failure.
  void fetchCalDavEvents(
      HttpClient& http, const CalDavAccount& account, std::chrono::system_clock::time_point start,
      std::chrono::system_clock::time_point end, bool allowRedirectAuth,
      std::function<void(bool ok, std::vector<CalendarEvent>)> cb
  );

} // namespace calendar
