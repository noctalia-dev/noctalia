#include "calendar/caldav_client.h"

#include "calendar/ical_parser.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "time/time_format.h"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace calendar {

  namespace {
    constexpr Logger kLog("calendar-caldav");

    std::string secretString(const std::shared_ptr<const security::SecureBuffer>& value) {
      if (!value) {
        return {};
      }
      const auto bytes = value->bytes();
      return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    std::string
    buildReportBody(std::chrono::system_clock::time_point start, std::chrono::system_clock::time_point end) {
      const std::string s = formatUtcTime(start, "%Y%m%dT%H%M%SZ");
      const std::string e = formatUtcTime(end, "%Y%m%dT%H%M%SZ");
      return "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
             "<C:calendar-query xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\">"
             "<D:prop><C:calendar-data><C:expand start=\""
          + s
          + "\" end=\""
          + e
          + "\"/></C:calendar-data></D:prop>"
            "<C:filter><C:comp-filter name=\"VCALENDAR\"><C:comp-filter name=\"VEVENT\">"
            "<C:time-range start=\""
          + s
          + "\" end=\""
          + e
          + "\"/></C:comp-filter></C:comp-filter></C:filter>"
            "</C:calendar-query>";
    }

    // First value of an iCalendar property (e.g. "UID", "SUMMARY") of the first VEVENT, for log messages.
    // Searching from BEGIN:VEVENT skips the DTSTART and RRULE lines of a preceding VTIMEZONE.
    std::string icsProperty(const std::string& ics, std::string_view name) {
      const std::size_t eventStart = ics.find("BEGIN:VEVENT");
      std::size_t pos = eventStart == std::string::npos ? 0 : eventStart;
      while ((pos = ics.find(name, pos)) != std::string::npos) {
        const bool lineStart = pos == 0 || ics[pos - 1] == '\n';
        const std::size_t after = pos + name.size();
        if (lineStart && after < ics.size() && (ics[after] == ':' || ics[after] == ';')) {
          const std::size_t colon = ics.find(':', after);
          const std::size_t eol = ics.find_first_of("\r\n", colon);
          if (colon == std::string::npos) {
            return {};
          }
          return ics.substr(colon + 1, eol == std::string::npos ? std::string::npos : eol - colon - 1);
        }
        pos = after;
      }
      return {};
    }

    // Collect the text of every element whose local name is "calendar-data".
    // libxml2 stores the local name in node->name; the namespace prefix lives in node->ns.
    void collectCalendarData(xmlNode* node, std::vector<std::string>& out) {
      if (node->type == XML_ELEMENT_NODE
          && std::strcmp(reinterpret_cast<const char*>(node->name), "calendar-data") == 0) {
        xmlChar* content = xmlNodeGetContent(node);
        if (content != nullptr) {
          if (content[0] != '\0') {
            out.emplace_back(reinterpret_cast<const char*>(content));
          }
          xmlFree(content);
        }
      }
      for (xmlNode* child = node->children; child != nullptr; child = child->next) {
        collectCalendarData(child, out);
      }
    }
  } // namespace

  struct CalDavClient::State : std::enable_shared_from_this<CalDavClient::State> {
    struct ParseRequest {
      std::string body;
      std::string calendarName;
      std::string color;
      std::chrono::system_clock::time_point start;
      std::chrono::system_clock::time_point end;
      EventCallback callback;
    };

    State() : worker([this](const std::stop_token& stopToken) { workerLoop(stopToken); }) {}

    ~State() { stop(); }

    bool enqueue(ParseRequest request) {
      {
        std::scoped_lock lock(mutex);
        if (stopping) {
          return false;
        }
        requests.push_back(std::move(request));
      }
      cv.notify_one();
      return true;
    }

    void deliver(EventCallback callback, SyncFailure failure, std::vector<CalendarEvent> events) {
      const std::weak_ptr<State> weak = weak_from_this();
      DeferredCall::callLater([weak, callback = std::move(callback), failure, events = std::move(events)] mutable {
        const auto state = weak.lock();
        if (!state || !state->accepting()) {
          return;
        }
        callback(failure, std::move(events));
      });
    }

    void stop() {
      worker.request_stop();
      {
        std::scoped_lock lock(mutex);
        stopping = true;
        requests.clear();
      }
      cv.notify_one();
      if (worker.joinable()) {
        worker.join();
      }
    }

  private:
    bool accepting() const {
      std::scoped_lock lock(mutex);
      return !stopping;
    }

    static std::pair<SyncFailure, std::vector<CalendarEvent>>
    parse(const ParseRequest& request, std::stop_token stopToken) {
      xmlDocPtr doc = xmlReadMemory(
          request.body.data(), static_cast<int>(request.body.size()), "caldav.xml", nullptr,
          XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_RECOVER
      );
      if (doc == nullptr) {
        kLog.warn("caldav response XML parse error");
        return {SyncFailure::InvalidResponse, {}};
      }

      std::vector<std::string> calendarDataBlocks;
      if (xmlNode* root = xmlDocGetRootElement(doc); root != nullptr) {
        collectCalendarData(root, calendarDataBlocks);
      }
      xmlFreeDoc(doc);

      ICalParseControl control{.stopToken = std::move(stopToken)};

      std::vector<CalendarEvent> events;
      std::size_t skipped = 0;
      for (const std::string& ics : calendarDataBlocks) {
        ICalParseResult result = parseICalEvents(ics, request.start, request.end, control);
        if (result.status == ICalParseStatus::Cancelled) {
          return {SyncFailure::Cancelled, {}};
        }
        if (result.status == ICalParseStatus::InvalidCalendar) {
          // One unusable resource must not hide the whole collection: skip it and say why.
          ++skipped;
          const std::string uid = icsProperty(ics, "UID");
          kLog.warn("caldav calendar '{}': skipping invalid resource uid={}", request.calendarName, uid);
          kLog.debug(
              "caldav calendar '{}': skipped resource uid={} summary='{}' dtstart={} rrule={} status={} ({} bytes)",
              request.calendarName, uid, icsProperty(ics, "SUMMARY"), icsProperty(ics, "DTSTART"),
              icsProperty(ics, "RRULE"), icsProperty(ics, "STATUS"), ics.size()
          );
          continue;
        }
        if (result.status != ICalParseStatus::Complete) {
          if (result.status == ICalParseStatus::WorkBudgetExceeded) {
            kLog.warn(
                "caldav calendar '{}': recurrence work budget exceeded at event uid={}", request.calendarName,
                icsProperty(ics, "UID")
            );
          }
          return {SyncFailure::InvalidResponse, {}};
        }
        for (CalendarEvent& event : result.events) {
          event.calendarName = request.calendarName;
          if (event.colorHex.empty() && !request.color.empty()) {
            event.colorHex = request.color;
          }
          events.push_back(std::move(event));
        }
      }
      if (!calendarDataBlocks.empty() && skipped == calendarDataBlocks.size()) {
        // Nothing usable at all: report a failed refresh instead of an empty calendar, matching the
        // InvalidCalendar contract of parseICalEvents().
        kLog.warn(
            "caldav calendar '{}': all {} resource(s) were invalid; reporting the calendar as failed",
            request.calendarName, calendarDataBlocks.size()
        );
        return {SyncFailure::InvalidResponse, {}};
      }
      kLog.debug(
          "caldav calendar '{}': {} resource(s), {} event(s), {} skipped", request.calendarName,
          calendarDataBlocks.size(), events.size(), skipped
      );
      return {SyncFailure::None, std::move(events)};
    }

    void workerLoop(const std::stop_token& stopToken) {
      while (true) {
        ParseRequest request;
        {
          std::unique_lock lock(mutex);
          cv.wait(lock, [this] { return stopping || !requests.empty(); });
          if (stopping) {
            return;
          }
          request = std::move(requests.front());
          requests.pop_front();
        }

        auto [failure, events] = parse(request, stopToken);
        if (stopToken.stop_requested()) {
          return;
        }
        deliver(std::move(request.callback), failure, std::move(events));
      }
    }

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<ParseRequest> requests;
    bool stopping = false;
    std::jthread worker;
  };

  CalDavClient::CalDavClient(HttpClient& http)
      : CalDavClient([&http](HttpRequest request, ResponseCallback callback) {
          http.request(std::move(request), std::move(callback));
        }) {}

  CalDavClient::CalDavClient(RequestFunction request)
      : m_request(std::move(request)), m_state(std::make_shared<State>()) {}

  CalDavClient::~CalDavClient() {
    if (m_state) {
      m_state->stop();
      m_state.reset();
    }
  }

  void CalDavClient::fetchEvents(
      const CalDavAccount& account, std::chrono::system_clock::time_point start,
      std::chrono::system_clock::time_point end, bool allowRedirectAuth, EventCallback cb
  ) {
    HttpRequest req;
    req.method = "REPORT";
    req.url = account.url;
    req.body = buildReportBody(start, end);
    req.followRedirects = true;
    req.allowRedirectAuth = allowRedirectAuth;
    req.basicUsername = account.username;
    req.basicPassword = secretString(account.password);
    req.tlsClientCert = account.tls;
    req.headers = {
        "Depth: 1",
        "Content-Type: application/xml; charset=utf-8",
    };

    const std::string calendarName = account.calendarName;
    const std::string color = account.color;
    const std::weak_ptr<State> weak = m_state;
    m_request(std::move(req), [weak, cb = std::move(cb), calendarName, color, start, end](HttpResponse resp) mutable {
      const auto state = weak.lock();
      if (!state) {
        return;
      }
      if (!resp.transportOk || (resp.status != 207 && resp.status != 200)) {
        kLog.warn("caldav REPORT for calendar '{}' failed http={}", calendarName, resp.status);
        state->deliver(std::move(cb), syncFailureFromResponse(resp), {});
        return;
      }
      state->enqueue({
          .body = std::move(resp.body),
          .calendarName = calendarName,
          .color = color,
          .start = start,
          .end = end,
          .callback = std::move(cb),
      });
    });
  }

} // namespace calendar
