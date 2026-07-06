#include "calendar/google_client.h"

#include "core/log.h"
#include "net/http_client.h"
#include "net/uri.h"
#include "time/time_format.h"

#include <charconv>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>

namespace calendar {

  namespace {
    constexpr Logger kLog("calendar-google");

    int toInt(std::string_view text) {
      int value = 0;
      std::from_chars(text.data(), text.data() + text.size(), value);
      return value;
    }

    std::chrono::system_clock::time_point toSystem(const std::chrono::sys_time<std::chrono::seconds>& t) {
      return std::chrono::time_point_cast<std::chrono::system_clock::duration>(t);
    }

    // All-day date "YYYY-MM-DD" -> local midnight.
    std::optional<std::chrono::system_clock::time_point> parseDate(std::string_view s) {
      using namespace std::chrono;
      if (s.size() < 10) {
        return std::nullopt;
      }
      const year_month_day ymd{
          std::chrono::year{toInt(s.substr(0, 4))}
          / std::chrono::month{static_cast<unsigned>(toInt(s.substr(5, 2)))}
          / std::chrono::day{static_cast<unsigned>(toInt(s.substr(8, 2)))}
      };
      if (!ymd.ok()) {
        return std::nullopt;
      }
      try {
        return toSystem(time_point_cast<seconds>(current_zone()->to_sys(local_days{ymd})));
      } catch (...) {
        return toSystem(sys_days{ymd});
      }
    }

    // RFC 3339 "YYYY-MM-DDTHH:MM:SS[.fff](Z|±HH:MM)" -> UTC time_point.
    std::optional<std::chrono::system_clock::time_point> parseDateTime(std::string_view s) {
      using namespace std::chrono;
      if (s.size() < 19 || s[10] != 'T') {
        return std::nullopt;
      }
      const year_month_day ymd{
          std::chrono::year{toInt(s.substr(0, 4))}
          / std::chrono::month{static_cast<unsigned>(toInt(s.substr(5, 2)))}
          / std::chrono::day{static_cast<unsigned>(toInt(s.substr(8, 2)))}
      };
      if (!ymd.ok()) {
        return std::nullopt;
      }
      const auto timeOfDay =
          hours{toInt(s.substr(11, 2))} + minutes{toInt(s.substr(14, 2))} + seconds{toInt(s.substr(17, 2))};
      auto utc = sys_days{ymd} + timeOfDay;

      // Locate the zone designator after the seconds (skipping optional fractional seconds).
      std::size_t pos = 19;
      if (pos < s.size() && s[pos] == '.') {
        ++pos;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
          ++pos;
        }
      }
      if (pos < s.size() && (s[pos] == '+' || s[pos] == '-') && pos + 5 < s.size() + 1) {
        const int sign = s[pos] == '+' ? 1 : -1;
        const int offMinutes = sign * (toInt(s.substr(pos + 1, 2)) * 60 + toInt(s.substr(pos + 4, 2)));
        utc -= minutes{offMinutes};
      }
      return toSystem(utc);
    }

    struct CalendarMeta {
      std::string id;
      std::string name;
      std::string color;
    };

    struct FetchContext {
      std::size_t pending = 0;
      bool unauthorized = false;
      std::vector<CalendarEvent> events;
      std::function<void(bool, bool, std::vector<CalendarEvent>)> cb;
    };

    void parseEventsInto(const nlohmann::json& j, const CalendarMeta& meta, std::vector<CalendarEvent>& out) {
      const auto items = j.find("items");
      if (items == j.end() || !items->is_array()) {
        return;
      }
      for (const auto& item : *items) {
        if (item.value("status", std::string{}) == "cancelled") {
          continue;
        }
        CalendarEvent event;
        event.id = item.value("id", std::string{});
        event.title = item.value("summary", std::string{});
        event.location = item.value("location", std::string{});
        event.calendarName = meta.name;
        event.colorHex = meta.color;

        const auto readEndpoint = [](const nlohmann::json& node, std::chrono::system_clock::time_point& tp,
                                     bool& allDay) -> bool {
          if (auto d = node.find("date"); d != node.end() && d->is_string()) {
            if (auto parsed = parseDate(d->get<std::string>())) {
              tp = *parsed;
              allDay = true;
              return true;
            }
          }
          if (auto dt = node.find("dateTime"); dt != node.end() && dt->is_string()) {
            if (auto parsed = parseDateTime(dt->get<std::string>())) {
              tp = *parsed;
              allDay = false;
              return true;
            }
          }
          return false;
        };

        const auto startNode = item.find("start");
        const auto endNode = item.find("end");
        bool startAllDay = false;
        bool endAllDay = false;
        if (startNode == item.end() || !readEndpoint(*startNode, event.start, startAllDay)) {
          continue;
        }
        if (endNode == item.end() || !readEndpoint(*endNode, event.end, endAllDay)) {
          event.end = event.start;
        }
        event.allDay = startAllDay;
        out.push_back(std::move(event));
      }
    }
  } // namespace

  GoogleClient::GoogleClient(HttpClient& http) : m_http(http) {}

  void GoogleClient::fetchEvents(
      const std::string& accessToken, std::chrono::system_clock::time_point start,
      std::chrono::system_clock::time_point end, std::function<void(bool, bool, std::vector<CalendarEvent>)> cb
  ) {
    const std::string bearer = "Authorization: Bearer " + accessToken;

    HttpRequest listReq;
    listReq.method = "GET";
    listReq.url = "https://www.googleapis.com/calendar/v3/users/me/calendarList";
    listReq.followRedirects = true;
    listReq.headers = {bearer};

    m_http.request(std::move(listReq), [this, bearer, start, end, cb = std::move(cb)](HttpResponse resp) mutable {
      if (resp.transportOk && resp.status == 401) {
        cb(false, true, {});
        return;
      }
      if (!resp.transportOk || resp.status != 200) {
        kLog.warn("calendarList failed http={}", resp.status);
        cb(false, false, {});
        return;
      }

      std::vector<CalendarMeta> calendars;
      try {
        const auto j = nlohmann::json::parse(resp.body);
        if (auto items = j.find("items"); items != j.end() && items->is_array()) {
          for (const auto& item : *items) {
            if (item.value("selected", true) == false) {
              continue;
            }
            CalendarMeta meta;
            meta.id = item.value("id", std::string{});
            meta.name = item.value("summaryOverride", item.value("summary", std::string{}));
            meta.color = item.value("backgroundColor", std::string{});
            if (!meta.id.empty()) {
              calendars.push_back(std::move(meta));
            }
          }
        }
      } catch (const std::exception& e) {
        kLog.warn("calendarList parse error: {}", e.what());
        cb(false, false, {});
        return;
      }

      if (calendars.empty()) {
        cb(true, false, {});
        return;
      }

      auto ctx = std::make_shared<FetchContext>();
      ctx->pending = calendars.size();
      ctx->cb = std::move(cb);

      const std::string timeMin = formatUtcTime(start, "%Y-%m-%dT%H:%M:%SZ");
      const std::string timeMax = formatUtcTime(end, "%Y-%m-%dT%H:%M:%SZ");
      for (const CalendarMeta& meta : calendars) {
        HttpRequest req;
        req.method = "GET";
        req.followRedirects = true;
        req.url = "https://www.googleapis.com/calendar/v3/calendars/"
            + uri::encodeComponent(meta.id)
            + "/events?singleEvents=true&orderBy=startTime&maxResults=2500&timeMin="
            + uri::encodeComponent(timeMin)
            + "&timeMax="
            + uri::encodeComponent(timeMax);
        req.headers = {bearer};
        m_http.request(std::move(req), [ctx, meta](HttpResponse evResp) {
          if (evResp.transportOk && evResp.status == 401) {
            ctx->unauthorized = true;
          } else if (evResp.transportOk && evResp.status == 200) {
            try {
              const auto j = nlohmann::json::parse(evResp.body);
              parseEventsInto(j, meta, ctx->events);
            } catch (const std::exception& e) {
              kLog.warn("events parse error for calendar {}: {}", meta.name, e.what());
            }
          } else {
            kLog.warn("events fetch failed http={}", evResp.status);
          }

          if (--ctx->pending == 0) {
            const bool ok = !ctx->unauthorized;
            ctx->cb(ok, ctx->unauthorized, std::move(ctx->events));
          }
        });
      }
    });
  }

} // namespace calendar
