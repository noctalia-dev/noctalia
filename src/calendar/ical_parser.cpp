#include "calendar/ical_parser.h"

#include <charconv>
#include <optional>
#include <string>

namespace calendar {

  namespace {

    // Join RFC 5545 folded lines: a CRLF/LF followed by a single space or tab is a continuation.
    std::vector<std::string> unfold(std::string_view ics) {
      std::vector<std::string> lines;
      std::string current;
      std::size_t i = 0;
      while (i < ics.size()) {
        std::size_t eol = ics.find('\n', i);
        std::string_view raw = eol == std::string_view::npos ? ics.substr(i) : ics.substr(i, eol - i);
        if (!raw.empty() && raw.back() == '\r') {
          raw.remove_suffix(1);
        }
        if (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t')) {
          current.append(raw.substr(1));
        } else {
          if (!current.empty()) {
            lines.push_back(std::move(current));
          }
          current.assign(raw);
        }
        if (eol == std::string_view::npos) {
          break;
        }
        i = eol + 1;
      }
      if (!current.empty()) {
        lines.push_back(std::move(current));
      }
      return lines;
    }

    // Unescape TEXT values: \\n / \\N -> newline, \\, \\; \\\\ -> literal.
    std::string unescapeText(std::string_view value) {
      std::string out;
      out.reserve(value.size());
      for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
          const char next = value[++i];
          switch (next) {
          case 'n':
          case 'N':
            out.push_back('\n');
            break;
          default:
            out.push_back(next);
            break;
          }
        } else {
          out.push_back(value[i]);
        }
      }
      return out;
    }

    struct PropertyLine {
      std::string_view name;
      std::string_view tzid;
      bool valueIsDate = false;
      std::string_view value;
    };

    int toInt(std::string_view text) {
      int value = 0;
      std::from_chars(text.data(), text.data() + text.size(), value);
      return value;
    }

    PropertyLine parseProperty(std::string_view line) {
      PropertyLine prop;
      const std::size_t colon = line.find(':');
      if (colon == std::string_view::npos) {
        return prop;
      }
      std::string_view head = line.substr(0, colon);
      prop.value = line.substr(colon + 1);

      const std::size_t firstSemi = head.find(';');
      prop.name = head.substr(0, firstSemi);
      std::size_t pos = firstSemi;
      while (pos != std::string_view::npos) {
        const std::size_t start = pos + 1;
        const std::size_t nextSemi = head.find(';', start);
        std::string_view param =
            nextSemi == std::string_view::npos ? head.substr(start) : head.substr(start, nextSemi - start);
        const std::size_t eq = param.find('=');
        if (eq != std::string_view::npos) {
          std::string_view key = param.substr(0, eq);
          std::string_view val = param.substr(eq + 1);
          if (key == "TZID") {
            prop.tzid = val;
          } else if (key == "VALUE" && val == "DATE") {
            prop.valueIsDate = true;
          }
        }
        pos = nextSemi;
      }
      return prop;
    }

    std::chrono::system_clock::time_point toSystem(const std::chrono::sys_time<std::chrono::seconds>& t) {
      return std::chrono::time_point_cast<std::chrono::system_clock::duration>(t);
    }

    // Parse an iCal DATE or DATE-TIME value into a UTC time_point. Sets allDay for DATE values.
    std::optional<std::chrono::system_clock::time_point> parseDateTime(const PropertyLine& prop, bool& allDay) {
      using namespace std::chrono;
      std::string_view v = prop.value;
      if (v.size() < 8) {
        return std::nullopt;
      }
      const int year = toInt(v.substr(0, 4));
      const auto month = static_cast<unsigned>(toInt(v.substr(4, 2)));
      const auto day = static_cast<unsigned>(toInt(v.substr(6, 2)));
      const year_month_day ymd{std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day}};
      if (!ymd.ok()) {
        return std::nullopt;
      }

      if (prop.valueIsDate || v.size() < 15 || (v[8] != 'T')) {
        allDay = true;
        // Local midnight of the date.
        const local_days ld{ymd};
        try {
          return toSystem(time_point_cast<seconds>(current_zone()->to_sys(ld)));
        } catch (...) {
          return toSystem(sys_days{ymd});
        }
      }

      allDay = false;
      const int hour = toInt(v.substr(9, 2));
      const int minute = toInt(v.substr(11, 2));
      const int second = toInt(v.substr(13, 2));
      const auto timeOfDay = hours{hour} + minutes{minute} + seconds{second};

      if (v.back() == 'Z') {
        return toSystem(sys_days{ymd} + timeOfDay);
      }

      const local_seconds local = local_days{ymd} + timeOfDay;
      if (!prop.tzid.empty()) {
        try {
          return toSystem(time_point_cast<seconds>(locate_zone(std::string(prop.tzid))->to_sys(local)));
        } catch (...) {
          // Fall through to the local zone when the TZID is unknown.
        }
      }
      try {
        return toSystem(time_point_cast<seconds>(current_zone()->to_sys(local)));
      } catch (...) {
        return toSystem(sys_days{ymd} + timeOfDay);
      }
    }

  } // namespace

  std::vector<CalendarEvent> parseICalEvents(std::string_view ics) {
    std::vector<CalendarEvent> events;
    const std::vector<std::string> lines = unfold(ics);

    bool inEvent = false;
    CalendarEvent event;
    bool haveStart = false;
    bool haveEnd = false;
    bool startAllDay = false;
    bool endAllDay = false;

    for (const std::string& line : lines) {
      if (line == "BEGIN:VEVENT") {
        inEvent = true;
        event = CalendarEvent{};
        haveStart = haveEnd = startAllDay = endAllDay = false;
        continue;
      }
      if (line == "END:VEVENT") {
        if (inEvent && haveStart) {
          if (!haveEnd) {
            event.end = event.start;
          }
          event.allDay = startAllDay;
          events.push_back(std::move(event));
        }
        inEvent = false;
        continue;
      }
      if (!inEvent) {
        continue;
      }

      const PropertyLine prop = parseProperty(line);
      if (prop.name == "UID") {
        event.id = std::string(prop.value);
      } else if (prop.name == "SUMMARY") {
        event.title = unescapeText(prop.value);
      } else if (prop.name == "LOCATION") {
        event.location = unescapeText(prop.value);
      } else if (prop.name == "DTSTART") {
        if (auto tp = parseDateTime(prop, startAllDay)) {
          event.start = *tp;
          haveStart = true;
        }
      } else if (prop.name == "DTEND") {
        if (auto tp = parseDateTime(prop, endAllDay)) {
          event.end = *tp;
          haveEnd = true;
        }
      }
    }

    return events;
  }

} // namespace calendar
