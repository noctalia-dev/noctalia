#include "calendar/ical_parser.h"

#include "calendar/event_link.h"
#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <libical/ical.h>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace calendar {

  namespace {
    constexpr Logger kLog("ical-parser");

    struct ICalComponentDeleter {
      void operator()(icalcomponent* component) const {
        if (component != nullptr) {
          icalcomponent_free(component);
        }
      }
    };

    using ICalComponentPtr = std::unique_ptr<icalcomponent, ICalComponentDeleter>;

    struct ICalRecurDeleter {
      void operator()(icalrecur_iterator* iterator) const {
        if (iterator != nullptr) {
          icalrecur_iterator_free(iterator);
        }
      }
    };

    using ICalRecurPtr = std::unique_ptr<icalrecur_iterator, ICalRecurDeleter>;

    int durationSeconds(const icaldurationtype& duration) {
#if ICAL_CHECK_VERSION(4, 0, 0)
      return icaldurationtype_as_seconds(duration);
#else
      return icaldurationtype_as_int(duration);
#endif
    }

    std::chrono::system_clock::time_point toSystem(std::chrono::sys_time<std::chrono::seconds> t) {
      return std::chrono::time_point_cast<std::chrono::system_clock::duration>(t);
    }

    std::chrono::system_clock::time_point fromUnix(time_t seconds) {
      return std::chrono::system_clock::time_point{std::chrono::seconds{seconds}};
    }

    time_t toUnix(std::chrono::system_clock::time_point t) {
      return static_cast<time_t>(std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count());
    }

    std::chrono::system_clock::time_point localMidnight(const icaltimetype& t) {
      using namespace std::chrono;
      const year_month_day ymd{
          year{t.year} / month{static_cast<unsigned>(t.month)} / day{static_cast<unsigned>(t.day)}
      };
      if (!ymd.ok()) {
        return {};
      }

      try {
        return toSystem(time_point_cast<seconds>(current_zone()->to_sys(local_days{ymd})));
      } catch (...) {
        return toSystem(sys_days{ymd});
      }
    }

    std::chrono::system_clock::time_point localDateTime(int y, int mo, int d, int h, int mi, int s) {
      using namespace std::chrono;
      const year_month_day ymd{year{y} / month{static_cast<unsigned>(mo)} / day{static_cast<unsigned>(d)}};
      if (!ymd.ok()) {
        return {};
      }

      const local_seconds local = local_days{ymd} + hours{h} + minutes{mi} + seconds{s};
      try {
        return toSystem(time_point_cast<seconds>(current_zone()->to_sys(local)));
      } catch (...) {
        return toSystem(sys_days{ymd} + hours{h} + minutes{mi} + seconds{s});
      }
    }

    std::chrono::system_clock::time_point localDateTime(const icaltimetype& t) {
      return localDateTime(t.year, t.month, t.day, t.hour, t.minute, t.second);
    }

    std::chrono::system_clock::time_point localDateTimeFromUnixWallTime(time_t unixSeconds) {
      using namespace std::chrono;
      const sys_seconds wall{seconds{unixSeconds}};
      const sys_days day = floor<days>(wall);
      const year_month_day ymd{day};
      const hh_mm_ss time{wall - day};
      return localDateTime(
          static_cast<int>(ymd.year()), static_cast<int>(static_cast<unsigned>(ymd.month())),
          static_cast<int>(static_cast<unsigned>(ymd.day())), static_cast<int>(time.hours().count()),
          static_cast<int>(time.minutes().count()), static_cast<int>(time.seconds().count())
      );
    }

    bool isFloatingDateTime(const icaltimetype& t) {
      return !icaltime_is_date(t) && icaltime_is_utc(t) == 0 && icaltime_get_timezone(t) == nullptr;
    }

    std::chrono::system_clock::time_point localMidnightFromUnixDate(time_t unixSeconds) {
      using namespace std::chrono;
      const sys_days day = floor<days>(sys_seconds{std::chrono::seconds{unixSeconds}});
      const year_month_day ymd{day};
      icaltimetype t = icaltime_null_date();
      t.year = static_cast<int>(ymd.year());
      t.month = static_cast<int>(static_cast<unsigned>(ymd.month()));
      t.day = static_cast<int>(static_cast<unsigned>(ymd.day()));
      t.is_date = 1;
      return localMidnight(t);
    }

    std::chrono::system_clock::time_point timePointFromICal(icaltimetype t) {
      if (icaltime_is_null_time(t) || !icaltime_is_valid_time(t)) {
        return {};
      }
      if (icaltime_is_date(t)) {
        return localMidnight(t);
      }
      if (isFloatingDateTime(t)) {
        return localDateTime(t);
      }
      const icaltimetype utc = icaltime_convert_to_zone(t, icaltimezone_get_utc_timezone());
      return fromUnix(icaltime_as_timet(utc));
    }

    icaltimetype utcICalTime(std::chrono::system_clock::time_point t) {
      return icaltime_from_timet_with_zone(toUnix(t), 0, icaltimezone_get_utc_timezone());
    }

    icaltimetype localICalTime(std::chrono::system_clock::time_point t) {
      using namespace std::chrono;
      sys_seconds wallTime = floor<seconds>(t);
      try {
        const local_seconds local = floor<seconds>(current_zone()->to_local(t));
        wallTime = sys_seconds{local.time_since_epoch()};
      } catch (...) {
        // Keep UTC wall fields when the system timezone database is unavailable.
      }

      const sys_days day = floor<days>(wallTime);
      const year_month_day ymd{day};
      const hh_mm_ss time{wallTime - day};
      icaltimetype result = icaltime_null_time();
      result.year = static_cast<int>(ymd.year());
      result.month = static_cast<int>(static_cast<unsigned>(ymd.month()));
      result.day = static_cast<int>(static_cast<unsigned>(ymd.day()));
      result.hour = static_cast<int>(time.hours().count());
      result.minute = static_cast<int>(time.minutes().count());
      result.second = static_cast<int>(time.seconds().count());
      return result;
    }

    icaltimetype recurrenceBound(std::chrono::system_clock::time_point t, const icaltimetype& recurrenceStart) {
      if (icaltime_is_date(recurrenceStart) || isFloatingDateTime(recurrenceStart)) {
        return localICalTime(t);
      }
      return utcICalTime(t);
    }

    bool hasProperty(icalcomponent* component, icalproperty_kind kind) {
      return icalcomponent_get_first_property(component, kind) != nullptr;
    }

    bool isCancelled(icalcomponent* component) { return icalcomponent_get_status(component) == ICAL_STATUS_CANCELLED; }

    bool hasValidStart(icalcomponent* component) {
      if (!hasProperty(component, ICAL_DTSTART_PROPERTY)) {
        return false;
      }
      const icaltimetype start = icalcomponent_get_dtstart(component);
      return !icaltime_is_null_time(start) && icaltime_is_valid_time(start);
    }

    bool isExcluded(
        std::chrono::system_clock::time_point occurrence,
        const std::vector<std::chrono::system_clock::time_point>& exclusions
    ) {
      return std::ranges::contains(exclusions, occurrence);
    }

    CalendarEvent baseEventFromComponent(icalcomponent* component) {
      CalendarEvent event;
      if (const char* uid = icalcomponent_get_uid(component); uid != nullptr) {
        event.id = uid;
      }
      if (const char* summary = icalcomponent_get_summary(component); summary != nullptr) {
        event.title = summary;
      }
      if (const char* location = icalcomponent_get_location(component); location != nullptr) {
        event.location = location;
      }
      std::string urlProperty;
      if (icalproperty* url = icalcomponent_get_first_property(component, ICAL_URL_PROPERTY); url != nullptr) {
        if (const char* value = icalproperty_get_url(url); value != nullptr) {
          urlProperty = value;
        }
      }
      event.url = resolveEventLink(event.location, urlProperty);

      const icaltimetype start = icalcomponent_get_dtstart(component);
      const icaltimetype end = icalcomponent_get_dtend(component);
      event.start = timePointFromICal(start);
      event.end = timePointFromICal(end);
      event.allDay = icaltime_is_date(start) != 0;
      return event;
    }

    std::optional<std::chrono::system_clock::time_point> recurrenceId(icalcomponent* component) {
      if (!hasProperty(component, ICAL_RECURRENCEID_PROPERTY)) {
        return std::nullopt;
      }
      const icaltimetype id = icalcomponent_get_recurrenceid(component);
      if (icaltime_is_null_time(id) || !icaltime_is_valid_time(id)) {
        return std::nullopt;
      }
      return timePointFromICal(id);
    }

    void collectVEvents(icalcomponent* component, std::vector<icalcomponent*>& out) {
      if (component == nullptr) {
        return;
      }
      if (icalcomponent_isa(component) == ICAL_VEVENT_COMPONENT) {
        out.push_back(component);
        return;
      }
      for (icalcomponent* child = icalcomponent_get_first_component(component, ICAL_ANY_COMPONENT); child != nullptr;
           child = icalcomponent_get_next_component(component, ICAL_ANY_COMPONENT)) {
        collectVEvents(child, out);
      }
    }

    struct RecurrenceData {
      CalendarEvent base;
      std::chrono::system_clock::time_point windowStart;
      std::chrono::system_clock::time_point windowEnd;
      bool floatingDateTime = false;
      const std::vector<std::chrono::system_clock::time_point>* exclusions = nullptr;
      std::vector<CalendarEvent>* events = nullptr;
      std::unordered_map<time_t, std::size_t> eventIndexByStart;
    };

    bool consumeRecurrenceWork(ICalParseControl& control) {
      if (control.stopToken.stop_requested() || control.remainingRecurrenceWork == 0) {
        return false;
      }
      --control.remainingRecurrenceWork;
      return true;
    }

    time_t recurrenceUnixTime(const icaltimetype& time) {
      const icaltimezone* zone = icaltime_get_timezone(time);
      return icaltime_as_timet_with_zone(time, zone != nullptr ? zone : icaltimezone_get_utc_timezone());
    }

    void addRecurrence(const icaltime_span& span, RecurrenceData& data, bool preferExplicitDuration) {
      CalendarEvent event = data.base;
      if (event.allDay) {
        event.start = localMidnightFromUnixDate(span.start);
        event.end = localMidnightFromUnixDate(span.end);
      } else if (data.floatingDateTime) {
        event.start = localDateTimeFromUnixWallTime(span.start);
        event.end = localDateTimeFromUnixWallTime(span.end);
      } else {
        event.start = fromUnix(span.start);
        event.end = fromUnix(span.end);
      }
      if (event.start < data.windowStart || event.start > data.windowEnd) {
        return;
      }
      if (data.exclusions != nullptr && isExcluded(event.start, *data.exclusions)) {
        return;
      }

      const time_t start = toUnix(event.start);
      if (auto it = data.eventIndexByStart.find(start); it != data.eventIndexByStart.end()) {
        if (preferExplicitDuration) {
          (*data.events)[it->second] = std::move(event);
        }
        return;
      }
      data.eventIndexByStart.emplace(start, data.events->size());
      data.events->push_back(std::move(event));
    }

    ICalParseStatus expandRecurrences(
        icalcomponent* component, const icaltimetype& componentStart, ICalParseControl& control, RecurrenceData& data
    ) {
      const icaltimetype componentEnd = icalcomponent_get_dtend(component);
      const icaltime_span baseSpan = icaltime_span_new(componentStart, componentEnd, 1);
      const time_t duration = baseSpan.end - baseSpan.start;
      const icaltimetype endBound = recurrenceBound(data.windowEnd, componentStart);

      icalproperty* ruleProperty = icalcomponent_get_first_property(component, ICAL_RRULE_PROPERTY);
      if (ruleProperty == nullptr) {
        icaltimetype start = componentStart;
        if (icalproperty_recurrence_is_excluded(component, &start, &start) == 0) {
          addRecurrence(baseSpan, data, false);
        }
      }

      for (; ruleProperty != nullptr; ruleProperty = icalcomponent_get_next_property(component, ICAL_RRULE_PROPERTY)) {
#if ICAL_CHECK_VERSION(4, 0, 0)
        icalrecurrencetype* rule = icalproperty_get_rrule(ruleProperty);
        if (rule == nullptr) {
          continue;
        }
        ICalRecurPtr iterator{icalrecur_iterator_new(rule, componentStart)};
        const bool unbounded = rule->count == 0;
#else
        const icalrecurrencetype rule = icalproperty_get_rrule(ruleProperty);
        ICalRecurPtr iterator{icalrecur_iterator_new(rule, componentStart)};
        const bool unbounded = rule.count == 0;
#endif
        if (!iterator) {
          continue;
        }
        if (unbounded) {
          (void)icalrecur_iterator_set_start(iterator.get(), recurrenceBound(data.windowStart, componentStart));
        }

        while (!control.stopToken.stop_requested()) {
          icaltimetype occurrence = icalrecur_iterator_next(iterator.get());
          if (icaltime_is_null_time(occurrence)) {
            break;
          }
          if (!consumeRecurrenceWork(control)) {
            return control.stopToken.stop_requested() ? ICalParseStatus::Cancelled
                                                      : ICalParseStatus::WorkBudgetExceeded;
          }
          if (icaltime_compare(occurrence, endBound) > 0) {
            break;
          }

          icaltimetype start = componentStart;
          if (icalproperty_recurrence_is_excluded(component, &start, &occurrence) != 0) {
            continue;
          }
          const time_t occurrenceStart = recurrenceUnixTime(occurrence);
          addRecurrence(
              icaltime_span{.start = occurrenceStart, .end = occurrenceStart + duration, .is_busy = 1}, data, false
          );
        }
        if (control.stopToken.stop_requested()) {
          return ICalParseStatus::Cancelled;
        }
      }

      for (icalproperty* property = icalcomponent_get_first_property(component, ICAL_RDATE_PROPERTY);
           property != nullptr; property = icalcomponent_get_next_property(component, ICAL_RDATE_PROPERTY)) {
        if (!consumeRecurrenceWork(control)) {
          return control.stopToken.stop_requested() ? ICalParseStatus::Cancelled : ICalParseStatus::WorkBudgetExceeded;
        }

        const icaldatetimeperiodtype rdate = icalproperty_get_rdate(property);
        const bool explicitPeriod = icaltime_is_null_time(rdate.time);
        const icaltimetype occurrence = explicitPeriod ? rdate.period.start : rdate.time;
        if (icaltime_is_null_time(occurrence)) {
          continue;
        }

        icaltimetype start = componentStart;
        icaltimetype recurrence = occurrence;
        if (icalproperty_recurrence_is_excluded(component, &start, &recurrence) != 0) {
          continue;
        }

        const time_t occurrenceStart = recurrenceUnixTime(occurrence);
        time_t occurrenceEnd = occurrenceStart + duration;
        if (explicitPeriod) {
          if (!icaltime_is_null_time(rdate.period.end)) {
            occurrenceEnd = recurrenceUnixTime(rdate.period.end);
          } else {
            occurrenceEnd = occurrenceStart + static_cast<time_t>(durationSeconds(rdate.period.duration));
          }
        }
        addRecurrence(
            icaltime_span{.start = occurrenceStart, .end = occurrenceEnd, .is_busy = 1}, data, explicitPeriod
        );
      }

      return control.stopToken.stop_requested() ? ICalParseStatus::Cancelled : ICalParseStatus::Complete;
    }

  } // namespace

  ICalParseResult parseICalEvents(
      std::string_view ics, std::chrono::system_clock::time_point windowStart,
      std::chrono::system_clock::time_point windowEnd, ICalParseControl& control
  ) {
    std::string text{ics};
    ICalComponentPtr root{icalcomponent_new_from_string(text.c_str())};
    if (root == nullptr) {
      // Not iCalendar text at all: HTML sign-in pages, proxy errors, and truncated feeds all land
      // here. Reported as invalid so callers keep their last known events instead of persisting an
      // empty calendar. A well-formed VCALENDAR with no VEVENTs still parses and reports Complete.
      return {.status = ICalParseStatus::InvalidCalendar};
    }

    // libical recovers from content lines it cannot parse by replacing each one with an X-LIC-ERROR
    // property instead of failing the whole document. That keeps one quirky line in an otherwise
    // good feed from hiding every event in it, so a non-zero count alone is not a rejection.
    const int unparseableLines = icalcomponent_count_errors(root.get());
    if (unparseableLines > 0) {
      kLog.debug("iCalendar input has {} unparseable line(s); skipping them", unparseableLines);
    }

    std::vector<icalcomponent*> components;
    collectVEvents(root.get(), components);

    std::unordered_map<std::string, std::vector<std::chrono::system_clock::time_point>> overrides;
    for (icalcomponent* component : components) {
      if (control.stopToken.stop_requested()) {
        return {.status = ICalParseStatus::Cancelled};
      }
      const CalendarEvent event = baseEventFromComponent(component);
      const bool cancelledWithoutStart = isCancelled(component) && !hasProperty(component, ICAL_DTSTART_PROPERTY);
      if (auto id = recurrenceId(component); id && (hasValidStart(component) || cancelledWithoutStart)) {
        overrides[event.id].push_back(*id);
      }
    }

    std::vector<CalendarEvent> events;
    for (icalcomponent* component : components) {
      if (control.stopToken.stop_requested()) {
        return {.events = std::move(events), .status = ICalParseStatus::Cancelled};
      }
      if (isCancelled(component) || !hasValidStart(component)) {
        continue;
      }
      const icaltimetype componentStart = icalcomponent_get_dtstart(component);
      CalendarEvent event = baseEventFromComponent(component);

      if (!hasProperty(component, ICAL_RRULE_PROPERTY) && !hasProperty(component, ICAL_RDATE_PROPERTY)) {
        events.push_back(std::move(event));
        continue;
      }

      std::vector<std::chrono::system_clock::time_point> exclusions;
      if (auto it = overrides.find(event.id); it != overrides.end()) {
        exclusions = it->second;
      }
      RecurrenceData data{
          .base = std::move(event),
          .windowStart = windowStart,
          .windowEnd = windowEnd,
          .floatingDateTime = isFloatingDateTime(componentStart),
          .exclusions = &exclusions,
          .events = &events,
      };
      const ICalParseStatus status = expandRecurrences(component, componentStart, control, data);
      if (status != ICalParseStatus::Complete) {
        return {.events = std::move(events), .status = status};
      }
    }

    if (events.empty() && unparseableLines > 0) {
      // Damaged input that yielded nothing usable, as opposed to a calendar whose owner really has
      // no events. Reported as invalid so callers keep their cached events rather than persisting
      // an empty result.
      return {.status = ICalParseStatus::InvalidCalendar};
    }
    return {.events = std::move(events)};
  }

} // namespace calendar
