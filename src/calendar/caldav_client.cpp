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

    State() : worker([this](std::stop_token stopToken) { workerLoop(stopToken); }) {}

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

    void deliver(EventCallback callback, bool ok, std::vector<CalendarEvent> events) {
      const std::weak_ptr<State> weak = weak_from_this();
      DeferredCall::callLater([weak, callback = std::move(callback), ok, events = std::move(events)]() mutable {
        const auto state = weak.lock();
        if (!state || !state->accepting()) {
          return;
        }
        callback(ok, std::move(events));
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

    static std::pair<bool, std::vector<CalendarEvent>> parse(const ParseRequest& request, std::stop_token stopToken) {
      xmlDocPtr doc = xmlReadMemory(
          request.body.data(), static_cast<int>(request.body.size()), "caldav.xml", nullptr,
          XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_RECOVER
      );
      if (doc == nullptr) {
        kLog.warn("caldav response XML parse error");
        return {false, {}};
      }

      std::vector<std::string> calendarDataBlocks;
      if (xmlNode* root = xmlDocGetRootElement(doc); root != nullptr) {
        collectCalendarData(root, calendarDataBlocks);
      }
      xmlFreeDoc(doc);

      ICalParseControl control{.stopToken = stopToken};

      std::vector<CalendarEvent> events;
      for (const std::string& ics : calendarDataBlocks) {
        ICalParseResult result = parseICalEvents(ics, request.start, request.end, control);
        if (result.status != ICalParseStatus::Complete) {
          return {false, {}};
        }
        for (CalendarEvent& event : result.events) {
          event.calendarName = request.calendarName;
          event.colorHex = request.color;
          events.push_back(std::move(event));
        }
      }
      return {true, std::move(events)};
    }

    void workerLoop(std::stop_token stopToken) {
      while (true) {
        ParseRequest request;
        {
          std::unique_lock lock(mutex);
          cv.wait(lock, [this]() { return stopping || !requests.empty(); });
          if (stopping) {
            return;
          }
          request = std::move(requests.front());
          requests.pop_front();
        }

        auto [ok, events] = parse(request, stopToken);
        if (stopToken.stop_requested()) {
          return;
        }
        deliver(std::move(request.callback), ok, std::move(events));
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
        kLog.warn("caldav REPORT failed http={}", resp.status);
        state->deliver(std::move(cb), false, {});
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
