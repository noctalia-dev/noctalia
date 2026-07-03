#include "calendar/caldav_client.h"

#include "calendar/ical_parser.h"
#include "core/log.h"
#include "net/http_client.h"
#include "time/time_format.h"

#include <cstring>
#include <libxml/parser.h>
#include <libxml/tree.h>

namespace calendar {

  namespace {
    constexpr Logger kLog("calendar-caldav");

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

  void fetchCalDavEvents(
      HttpClient& http, const CalDavAccount& account, std::chrono::system_clock::time_point start,
      std::chrono::system_clock::time_point end, bool allowRedirectAuth,
      std::function<void(bool, std::vector<CalendarEvent>)> cb
  ) {
    HttpRequest req;
    req.method = "REPORT";
    req.url = account.url;
    req.body = buildReportBody(start, end);
    req.followRedirects = true;
    req.allowRedirectAuth = allowRedirectAuth;
    req.basicUsername = account.username;
    req.basicPassword = account.password;
    req.headers = {
        "Depth: 1",
        "Content-Type: application/xml; charset=utf-8",
    };

    const std::string calendarName = account.calendarName;
    const std::string color = account.color;
    http.request(std::move(req), [cb = std::move(cb), calendarName, color, start, end](HttpResponse resp) {
      if (!resp.transportOk || (resp.status != 207 && resp.status != 200)) {
        kLog.warn("caldav REPORT failed http={}", resp.status);
        cb(false, {});
        return;
      }

      xmlDocPtr doc = xmlReadMemory(
          resp.body.data(), static_cast<int>(resp.body.size()), "caldav.xml", nullptr,
          XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_RECOVER
      );
      if (doc == nullptr) {
        kLog.warn("caldav response XML parse error");
        cb(false, {});
        return;
      }

      std::vector<std::string> calendarDataBlocks;
      if (xmlNode* root = xmlDocGetRootElement(doc); root != nullptr) {
        collectCalendarData(root, calendarDataBlocks);
      }
      xmlFreeDoc(doc);

      std::vector<CalendarEvent> events;
      for (const std::string& ics : calendarDataBlocks) {
        for (CalendarEvent& event : parseICalEvents(ics, start, end)) {
          event.calendarName = calendarName;
          event.colorHex = color;
          events.push_back(std::move(event));
        }
      }
      cb(true, std::move(events));
    });
  }

} // namespace calendar
