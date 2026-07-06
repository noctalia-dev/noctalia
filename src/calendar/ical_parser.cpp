#include "calendar/ical_parser.h"

#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

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

    struct ParsedDateTime {
      std::chrono::system_clock::time_point instant;
      std::chrono::sys_days civilDay;
      std::chrono::seconds timeOfDay{0};
      const std::chrono::time_zone* zone = nullptr;
      bool allDay = false;
    };

    std::optional<ParsedDateTime> parseDateTime(const PropertyLine& prop) {
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
      const sys_days civilDay{ymd};

      if (prop.valueIsDate || v.size() < 15 || (v[8] != 'T')) {
        // Local midnight of the date.
        const local_days ld{ymd};
        try {
          return ParsedDateTime{
              toSystem(time_point_cast<seconds>(current_zone()->to_sys(ld))), civilDay, {}, nullptr, true
          };
        } catch (...) {
          return ParsedDateTime{toSystem(civilDay), civilDay, {}, nullptr, true};
        }
      }

      const int hour = toInt(v.substr(9, 2));
      const int minute = toInt(v.substr(11, 2));
      const int second = toInt(v.substr(13, 2));
      const auto timeOfDay = hours{hour} + minutes{minute} + seconds{second};

      if (v.back() == 'Z') {
        return ParsedDateTime{toSystem(civilDay + timeOfDay), civilDay, timeOfDay, nullptr, false};
      }

      const local_seconds local = local_days{ymd} + timeOfDay;
      if (!prop.tzid.empty()) {
        try {
          const time_zone* zone = locate_zone(std::string(prop.tzid));
          return ParsedDateTime{
              toSystem(time_point_cast<seconds>(zone->to_sys(local))), civilDay, timeOfDay, zone, false
          };
        } catch (...) {
          // Fall through to the local zone when the TZID is unknown.
        }
      }
      try {
        return ParsedDateTime{
            toSystem(time_point_cast<seconds>(current_zone()->to_sys(local))), civilDay, timeOfDay, nullptr, false
        };
      } catch (...) {
        return ParsedDateTime{toSystem(civilDay + timeOfDay), civilDay, timeOfDay, nullptr, false};
      }
    }

    // A numeric prefix ("2MO") is stripped and ignored: nth-of-period BYDAY is not supported.
    std::optional<std::chrono::weekday> parseWeekdayToken(std::string_view t) {
      while (!t.empty() && (t.front() == '+' || t.front() == '-' || (t.front() >= '0' && t.front() <= '9'))) {
        t.remove_prefix(1);
      }
      if (t == "SU")
        return std::chrono::Sunday;
      if (t == "MO")
        return std::chrono::Monday;
      if (t == "TU")
        return std::chrono::Tuesday;
      if (t == "WE")
        return std::chrono::Wednesday;
      if (t == "TH")
        return std::chrono::Thursday;
      if (t == "FR")
        return std::chrono::Friday;
      if (t == "SA")
        return std::chrono::Saturday;
      return std::nullopt;
    }

    struct RRule {
      enum class Freq { None, Daily, Weekly, Monthly, Yearly } freq = Freq::None;
      int interval = 1;
      int count = 0; // 0 = unbounded
      std::optional<std::chrono::system_clock::time_point> until;
      std::vector<std::chrono::weekday> byDay; // WEEKLY only
    };

    struct RecurrenceZone {
      const std::chrono::time_zone* zone = nullptr;
      std::chrono::sys_days startDay;
      std::chrono::seconds timeOfDay{0};
    };

    RRule parseRRule(std::string_view value) {
      RRule rule;
      std::size_t pos = 0;
      while (pos < value.size()) {
        const std::size_t semi = value.find(';', pos);
        std::string_view part = semi == std::string_view::npos ? value.substr(pos) : value.substr(pos, semi - pos);
        pos = semi == std::string_view::npos ? value.size() : semi + 1;

        const std::size_t eq = part.find('=');
        if (eq == std::string_view::npos) {
          continue;
        }
        const std::string_view key = part.substr(0, eq);
        const std::string_view val = part.substr(eq + 1);
        if (key == "FREQ") {
          if (val == "DAILY")
            rule.freq = RRule::Freq::Daily;
          else if (val == "WEEKLY")
            rule.freq = RRule::Freq::Weekly;
          else if (val == "MONTHLY")
            rule.freq = RRule::Freq::Monthly;
          else if (val == "YEARLY")
            rule.freq = RRule::Freq::Yearly;
        } else if (key == "INTERVAL") {
          rule.interval = std::max(1, toInt(val));
        } else if (key == "COUNT") {
          rule.count = toInt(val);
        } else if (key == "UNTIL") {
          PropertyLine p;
          p.value = val;
          if (auto parsed = parseDateTime(p)) {
            rule.until = parsed->instant;
          }
        } else if (key == "BYDAY") {
          std::size_t p = 0;
          while (p < val.size()) {
            const std::size_t comma = val.find(',', p);
            const std::string_view tok = comma == std::string_view::npos ? val.substr(p) : val.substr(p, comma - p);
            if (auto wd = parseWeekdayToken(tok)) {
              rule.byDay.push_back(*wd);
            }
            p = comma == std::string_view::npos ? val.size() : comma + 1;
          }
        }
      }
      return rule;
    }

    // Timed DTSTART values with TZID repeat at the same local wall time in that zone; UTC/floating
    // values keep the previous UTC-based expansion.
    void expandRecurrence(
        const CalendarEvent& base, const RRule& rule, const std::vector<std::chrono::system_clock::time_point>& exdates,
        const std::optional<RecurrenceZone>& recurrenceZone, std::chrono::system_clock::time_point windowStart,
        std::chrono::system_clock::time_point windowEnd, std::vector<CalendarEvent>& out
    ) {
      using namespace std::chrono;
      if (rule.freq == RRule::Freq::None) {
        out.push_back(base);
        return;
      }

      const auto duration = base.end - base.start;

      // For all-day (VALUE=DATE) events, base.start is local midnight stored as UTC, so flooring the
      // UTC instant to a day lands on the previous civil day for zones east of UTC. Recover the civil
      // date from the local zone and rebuild each occurrence's instant through it.
      const auto localDay = [](system_clock::time_point t) -> sys_days {
        try {
          return sys_days{year_month_day{floor<days>(current_zone()->to_local(t))}};
        } catch (...) {
          return floor<days>(t);
        }
      };
      sys_days startDay;
      days allDaySpan{0};
      system_clock::duration tod{0};
      if (base.allDay) {
        startDay = localDay(base.start);
        const sys_days endDay = localDay(base.end);
        allDaySpan = std::max(days{0}, endDay - startDay);
      } else if (recurrenceZone) {
        startDay = recurrenceZone->startDay;
        tod = recurrenceZone->timeOfDay;
      } else {
        startDay = floor<days>(base.start);
        tod = base.start - startDay;
      }

      const auto occInstant = [&](sys_days civilDay) -> system_clock::time_point {
        if (base.allDay) {
          const local_days ld{year_month_day{civilDay}};
          try {
            return toSystem(time_point_cast<seconds>(current_zone()->to_sys(ld)));
          } catch (...) {
            return toSystem(sys_days{year_month_day{civilDay}});
          }
        }
        if (recurrenceZone) {
          const local_seconds local = time_point_cast<seconds>(local_days{year_month_day{civilDay}} + tod);
          try {
            return toSystem(time_point_cast<seconds>(recurrenceZone->zone->to_sys(local)));
          } catch (...) {
            return civilDay + tod;
          }
        }
        return civilDay + tod;
      };

      // Legacy UTC-based occurrences can drift from the server's local-wall EXDATE/RECURRENCE-ID by a
      // DST offset. Occurrences are always >= 1 day apart, so a 12h tolerance matches the intended one
      // across that drift without catching a neighbour.
      const auto excluded = [&](system_clock::time_point t) {
        return std::ranges::any_of(exdates, [&](system_clock::time_point ex) {
          return std::chrono::abs(t - ex) < hours{12};
        });
      };
      int occurrenceNo = 0; // counts every occurrence (in or out of window) toward COUNT
      const auto step = [&](sys_days occDay) -> bool {
        const system_clock::time_point occ = occInstant(occDay);
        if (rule.until && occ > *rule.until)
          return false;
        if (occ > windowEnd)
          return false;
        ++occurrenceNo;
        if (rule.count != 0 && occurrenceNo > rule.count)
          return false;
        if (occ >= windowStart && !excluded(occ)) {
          CalendarEvent ev = base;
          ev.start = occ;
          ev.end = base.allDay ? occInstant(occDay + allDaySpan) : occ + duration;
          out.push_back(std::move(ev));
        }
        return true;
      };

      constexpr int kMaxIterations = 4000; // backstop; the window is ~1 year each way

      switch (rule.freq) {
      case RRule::Freq::Daily: {
        // Skip straight to the window rather than stepping day by day from a possibly years-old DTSTART
        // and exhausting kMaxIterations before reaching it. The i0 skipped occurrences still count toward
        // COUNT, so seed occurrenceNo with them — otherwise a bounded old series would either be miscounted
        // or (without the skip) truncated to nothing.
        int i0 = 0;
        if (base.start < windowStart) {
          i0 = static_cast<int>(duration_cast<days>(windowStart - base.start).count() / rule.interval);
        }
        occurrenceNo = i0;
        for (int i = i0; i < i0 + kMaxIterations; ++i) {
          if (!step(startDay + days{i * rule.interval}))
            break;
        }
        break;
      }
      case RRule::Freq::Weekly: {
        std::vector<weekday> byDay = rule.byDay;
        if (byDay.empty()) {
          byDay.emplace_back(startDay);
        }
        std::ranges::sort(byDay, {}, [](weekday w) { return w.iso_encoding(); });
        const unsigned fromMonday = weekday{startDay}.iso_encoding() - 1;
        const sys_days baseMonday = startDay - days{fromMonday};
        bool stop = false;
        for (int k = 0; k < kMaxIterations && !stop; ++k) {
          const sys_days weekMonday = baseMonday + weeks{k * rule.interval};
          for (const weekday wd : byDay) {
            const sys_days occDay = weekMonday + days{wd.iso_encoding() - 1};
            const system_clock::time_point occ = occInstant(occDay);
            if (occ < base.start)
              continue; // days earlier in the first week than DTSTART
            if (!step(occDay)) {
              stop = true;
              break;
            }
          }
        }
        break;
      }
      case RRule::Freq::Monthly: {
        const year_month_day ymd0{startDay};
        const year_month base0{ymd0.year(), ymd0.month()};
        for (int m = 0; m < kMaxIterations; m += rule.interval) {
          const year_month ym = base0 + months{m};
          const year_month_day occYmd{ym.year(), ym.month(), ymd0.day()};
          if (!occYmd.ok())
            continue; // e.g. day 31 in a short month: RFC skips it
          if (!step(sys_days{occYmd}))
            break;
        }
        break;
      }
      case RRule::Freq::Yearly: {
        const year_month_day ymd0{startDay};
        for (int y = 0; y < kMaxIterations; y += rule.interval) {
          const year_month_day occYmd{ymd0.year() + years{y}, ymd0.month(), ymd0.day()};
          if (!occYmd.ok())
            continue; // Feb 29 in a non-leap year
          if (!step(sys_days{occYmd}))
            break;
        }
        break;
      }
      case RRule::Freq::None:
        break;
      }
    }

  } // namespace

  std::vector<CalendarEvent> parseICalEvents(
      std::string_view ics, std::chrono::system_clock::time_point windowStart,
      std::chrono::system_clock::time_point windowEnd
  ) {
    const std::vector<std::string> lines = unfold(ics);

    // A parsed VEVENT held until every event is seen: RECURRENCE-ID overrides may appear before or
    // after their master, so masters can't be expanded until the full set of overridden instants is known.
    struct Parsed {
      CalendarEvent event;
      std::string_view rrule;
      std::vector<std::chrono::system_clock::time_point> exdates;
      std::optional<std::chrono::system_clock::time_point> recurrenceId;
      std::optional<RecurrenceZone> recurrenceZone;
    };
    std::vector<Parsed> parsed;

    bool inEvent = false;
    CalendarEvent event;
    bool haveStart = false;
    bool haveEnd = false;
    bool startAllDay = false;
    std::string_view rrule;
    std::vector<std::chrono::system_clock::time_point> exdates;
    std::optional<std::chrono::system_clock::time_point> recurrenceId;
    std::optional<RecurrenceZone> recurrenceZone;

    for (const std::string& line : lines) {
      if (line == "BEGIN:VEVENT") {
        inEvent = true;
        event = CalendarEvent{};
        haveStart = haveEnd = startAllDay = false;
        rrule = {};
        exdates.clear();
        recurrenceId.reset();
        recurrenceZone.reset();
        continue;
      }
      if (line == "END:VEVENT") {
        if (inEvent && haveStart) {
          if (!haveEnd) {
            event.end = event.start;
          }
          event.allDay = startAllDay;
          parsed.push_back({std::move(event), rrule, std::move(exdates), recurrenceId, recurrenceZone});
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
        if (auto parsed = parseDateTime(prop)) {
          event.start = parsed->instant;
          startAllDay = parsed->allDay;
          haveStart = true;
          if (!parsed->allDay && parsed->zone != nullptr) {
            recurrenceZone = RecurrenceZone{parsed->zone, parsed->civilDay, parsed->timeOfDay};
          }
        }
      } else if (prop.name == "DTEND") {
        if (auto parsed = parseDateTime(prop)) {
          event.end = parsed->instant;
          haveEnd = true;
        }
      } else if (prop.name == "RRULE") {
        rrule = prop.value;
      } else if (prop.name == "RECURRENCE-ID") {
        if (auto parsed = parseDateTime(prop)) {
          recurrenceId = parsed->instant;
        }
      } else if (prop.name == "EXDATE") {
        // May be a comma-separated list; each shares the line's TZID/VALUE params.
        std::string_view v = prop.value;
        std::size_t p = 0;
        while (p < v.size()) {
          const std::size_t comma = v.find(',', p);
          PropertyLine ex = prop;
          ex.value = comma == std::string_view::npos ? v.substr(p) : v.substr(p, comma - p);
          if (auto parsed = parseDateTime(ex)) {
            exdates.push_back(parsed->instant);
          }
          p = comma == std::string_view::npos ? v.size() : comma + 1;
        }
      }
    }

    // A RECURRENCE-ID VEVENT is a modified instance: it replaces the master's occurrence at that
    // instant. Collect those instants per UID and exclude them from the master's expansion, so the
    // standalone override VEVENT is the only copy of that occurrence.
    std::unordered_map<std::string, std::vector<std::chrono::system_clock::time_point>> overrides;
    for (const Parsed& p : parsed) {
      if (p.recurrenceId) {
        overrides[p.event.id].push_back(*p.recurrenceId);
      }
    }

    std::vector<CalendarEvent> events;
    for (Parsed& p : parsed) {
      if (p.rrule.empty()) {
        events.push_back(std::move(p.event));
        continue;
      }
      // Server didn't honor <C:expand>; expand the RRULE ourselves.
      if (auto it = overrides.find(p.event.id); it != overrides.end()) {
        p.exdates.insert(p.exdates.end(), it->second.begin(), it->second.end());
      }
      expandRecurrence(p.event, parseRRule(p.rrule), p.exdates, p.recurrenceZone, windowStart, windowEnd, events);
    }

    return events;
  }

} // namespace calendar
