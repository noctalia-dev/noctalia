#include "time/time_format.h"

#include "i18n/i18n.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <format>
#include <langinfo.h>
#include <locale>
#include <optional>
#include <string>
#include <string_view>

namespace {

  std::string
  formatAgeSeconds(std::int64_t secs, std::optional<std::chrono::system_clock::time_point> calendarAfterSixDays) {
    using namespace std::chrono;
    secs = std::max<std::int64_t>(secs, 0);
    if (secs < 60) {
      return i18n::tr("time.relative.just-now");
    }
    if (secs < 3600) {
      const long mins = secs / 60;
      return i18n::trp("time.relative.minutes-ago", mins);
    }
    if (secs < 86400) {
      const long hrs = secs / 3600;
      return i18n::trp("time.relative.hours-ago", hrs);
    }
    if (secs < 7 * 86400) {
      const long days = secs / 86400;
      return i18n::trp("time.relative.days-ago", days);
    }
    if (calendarAfterSixDays.has_value()) {
      const std::time_t rawTime = std::chrono::system_clock::to_time_t(*calendarAfterSixDays);
      std::tm localTime{};
      localtime_r(&rawTime, &localTime);
      if (const std::string date = formatStrftime("%b %e", localTime); !date.empty()) {
        return date;
      }
    }
    const long days = static_cast<long>(secs / 86400);
    return i18n::trp("time.relative.days-ago", days);
  }

  std::string formatDurationUnit(std::string_view key, std::uint64_t count) {
    return i18n::trp(key, static_cast<long>(count));
  }

  std::string formatDurationParts(const std::string& first, const std::string& second) {
    return i18n::tr("time.duration.two-parts", "first", first, "second", second);
  }

  std::string formatDurationParts(const std::string& first, const std::string& second, const std::string& third) {
    return i18n::tr("time.duration.three-parts", "first", first, "second", second, "third", third);
  }

} // namespace

namespace {

  std::string normalizeFormatEscapes(std::string_view fmt) {
    std::string out;
    out.reserve(fmt.size());
    for (std::size_t i = 0; i < fmt.size(); ++i) {
      if (fmt[i] == '\\' && i + 1 < fmt.size() && fmt[i + 1] == 'n') {
        out.push_back('\n');
        ++i;
      } else {
        out.push_back(fmt[i]);
      }
    }
    return out;
  }

  bool shouldUseStrftimeCompat(std::string_view fmt) {
    return fmt.contains("%-") || (fmt.contains('%') && (!fmt.contains('{') || fmt.contains("{:")));
  }

  std::string formatStrftimeRaw(std::string_view fmt, const std::tm& tm) {
    std::string spec(fmt);
    std::size_t size = std::max<std::size_t>(64, spec.size() * 4 + 16);
    for (int attempt = 0; attempt < 6; ++attempt) {
      std::string buffer(size, '\0');
      std::tm copy = tm;
      const std::size_t written = std::strftime(buffer.data(), buffer.size(), spec.c_str(), &copy);
      if (written > 0 || spec.empty()) {
        buffer.resize(written);
        return buffer;
      }
      size *= 2;
    }
    return {};
  }

  std::string
  formatStrftimeWithUnixSeconds(std::string_view fmt, const std::tm& tm, std::optional<std::int64_t> unixSeconds) {
    if (!unixSeconds.has_value() || !fmt.contains("%s")) {
      return formatStrftimeRaw(fmt, tm);
    }

    std::string out;
    out.reserve(fmt.size() + 16);
    std::string chunk;
    chunk.reserve(fmt.size());

    for (std::size_t i = 0; i < fmt.size();) {
      if (fmt[i] != '%' || i + 1 >= fmt.size()) {
        chunk.push_back(fmt[i++]);
        continue;
      }

      if (fmt[i + 1] == '%') {
        chunk.append("%%");
        i += 2;
        continue;
      }

      if (fmt[i + 1] == 's') {
        out += formatStrftimeRaw(chunk, tm);
        chunk.clear();
        out += std::to_string(*unixSeconds);
        i += 2;
        continue;
      }

      chunk.push_back(fmt[i++]);
    }

    out += formatStrftimeRaw(chunk, tm);
    return out;
  }

  std::optional<std::string> formatStrftimeCompat(
      std::string_view fmt, const std::tm& local, std::optional<std::int64_t> unixSeconds = std::nullopt
  ) {
    if (!shouldUseStrftimeCompat(fmt)) {
      return std::nullopt;
    }
    if (!fmt.contains('{')) {
      return formatStrftimeWithUnixSeconds(fmt, local, unixSeconds);
    }

    std::string out;
    out.reserve(fmt.size());
    bool formattedField = false;
    for (std::size_t i = 0; i < fmt.size();) {
      if (fmt[i] == '{' && i + 1 < fmt.size() && fmt[i + 1] == '{') {
        out.push_back('{');
        i += 2;
        continue;
      }
      if (fmt[i] == '}' && i + 1 < fmt.size() && fmt[i + 1] == '}') {
        out.push_back('}');
        i += 2;
        continue;
      }
      if (fmt[i] != '{') {
        out.push_back(fmt[i++]);
        continue;
      }

      const std::size_t end = fmt.find('}', i + 1);
      if (end == std::string_view::npos) {
        return std::nullopt;
      }
      const std::string_view field = fmt.substr(i + 1, end - i - 1);
      const std::size_t colon = field.find(':');
      if (colon == std::string_view::npos) {
        return std::nullopt;
      }
      std::string_view spec = field.substr(colon + 1);
      const std::size_t firstPercent = spec.find('%');
      if (firstPercent == std::string_view::npos) {
        return std::nullopt;
      }
      spec.remove_prefix(firstPercent);
      out += formatStrftimeWithUnixSeconds(spec, local, unixSeconds);
      formattedField = true;
      i = end + 1;
    }

    if (!formattedField) {
      return std::nullopt;
    }
    return out;
  }

} // namespace

std::string formatLocalTime(const char* fmt) {
  using namespace std::chrono;
  const std::string normalizedFmt = normalizeFormatEscapes(fmt);
  const auto now = floor<seconds>(system_clock::now());
  const auto unixSeconds = duration_cast<seconds>(now.time_since_epoch()).count();
  const std::time_t raw = system_clock::to_time_t(now);
  std::tm localTm{};
  localtime_r(&raw, &localTm);
  if (auto compat = formatStrftimeCompat(normalizedFmt, localTm, unixSeconds)) {
    return *compat;
  }

  const auto local = current_zone()->to_local(now);
  try {
    return std::vformat(std::locale(""), normalizedFmt, std::make_format_args(local));
  } catch (...) {
    return normalizedFmt;
  }
}

std::string formatLocalUnixTime(std::int64_t unixSeconds, std::string_view fmt) {
  using namespace std::chrono;
  const std::string normalizedFmt = normalizeFormatEscapes(fmt);
  const auto tp = sys_seconds{seconds{unixSeconds}};
  const std::time_t raw = system_clock::to_time_t(tp);
  std::tm localTm{};
  localtime_r(&raw, &localTm);
  if (auto compat = formatStrftimeCompat(normalizedFmt, localTm, unixSeconds)) {
    return *compat;
  }

  const auto local = current_zone()->to_local(tp);
  try {
    return std::vformat(std::locale(""), normalizedFmt, std::make_format_args(local));
  } catch (...) {
    return normalizedFmt;
  }
}

std::string formatIsoTime(std::string_view isoTime, const char* fmt) {
  std::tm tm{};
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  if (std::sscanf(std::string(isoTime).c_str(), "%d-%d-%dT%d:%d", &year, &month, &day, &hour, &minute) < 5) {
    return std::string(isoTime);
  }
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_isdst = -1;
  const std::time_t unixSeconds = mktime(&tm);

  const std::string normalizedFmt = normalizeFormatEscapes(fmt);
  if (auto compat = formatStrftimeCompat(
          normalizedFmt, tm,
          unixSeconds == static_cast<std::time_t>(-1) ? std::nullopt : std::make_optional<std::int64_t>(unixSeconds)
      )) {
    return *compat;
  }

  using namespace std::chrono;
  const auto tp =
      sys_days{
          std::chrono::year{year}
          / std::chrono::month{static_cast<unsigned>(month)}
          / std::chrono::day{static_cast<unsigned>(day)}
      }
      + hours{hour}
      + minutes{minute};
  const auto local = local_seconds{tp.time_since_epoch()};
  try {
    return std::vformat(std::locale(""), normalizedFmt, std::make_format_args(local));
  } catch (...) {
    return normalizedFmt;
  }
}

std::string formatStrftime(std::string_view fmt, const std::tm& tm) { return formatStrftimeRaw(fmt, tm); }

std::string formatUtcTime(std::chrono::system_clock::time_point tp, std::string_view fmt) {
  const std::time_t raw = std::chrono::system_clock::to_time_t(tp);
  std::tm tm{};
  gmtime_r(&raw, &tm);
  return formatStrftimeWithUnixSeconds(fmt, tm, static_cast<std::int64_t>(raw));
}

int localeFirstDayOfWeek() {
  // The _NL_TIME_* week items are glibc-only; elsewhere we keep the Monday fallback.
#if defined(__GLIBC__)
  // WEEK_1STDAY is a YYYYMMDD anchor packed into the low 32 bits of the returned
  // pointer; FIRST_WEEKDAY is a 1-based offset from that anchor in the first byte.
  const char* firstWeekdayInfo = nl_langinfo(_NL_TIME_FIRST_WEEKDAY);
  const auto week1stday =
      static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(nl_langinfo(_NL_TIME_WEEK_1STDAY)));
  if (firstWeekdayInfo != nullptr && week1stday > 0) {
    const int firstWeekday = static_cast<unsigned char>(*firstWeekdayInfo);
    const int year = static_cast<int>(week1stday / 10000);
    const int month = static_cast<int>((week1stday / 100) % 100);
    const int day = static_cast<int>(week1stday % 100);
    const std::chrono::year_month_day anchor{
        std::chrono::year{year}
        / std::chrono::month{static_cast<unsigned>(month)}
        / std::chrono::day{static_cast<unsigned>(day)}
    };
    if (firstWeekday >= 1 && anchor.ok()) {
      const int anchorWeekday = static_cast<int>(std::chrono::weekday{std::chrono::sys_days{anchor}}.c_encoding());
      return (anchorWeekday + firstWeekday - 1) % 7;
    }
  }
#endif
  return 1; // ISO-style Monday when the platform does not expose locale week data.
}

std::string formatCurrentDate() {
  std::string fmt = "%A, ";
  fmt += nl_langinfo(D_FMT);
  for (std::size_t pos = 0; (pos = fmt.find("%y", pos)) != std::string::npos;) {
    fmt.replace(pos, 2, "%Y");
    pos += 2;
  }
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  return formatStrftime(fmt, local);
}

std::string formatClockTime(std::int64_t seconds) {
  if (seconds <= 0) {
    return "0:00";
  }
  const std::int64_t totalMinutes = seconds / 60;
  const std::int64_t hours = totalMinutes / 60;
  const std::int64_t minutes = totalMinutes % 60;
  const std::int64_t secs = seconds % 60;
  if (hours > 0) {
    return std::format("{}:{:02}:{:02}", hours, minutes, secs);
  }
  return std::format("{}:{:02}", minutes, secs);
}

std::string formatFileTime(const std::filesystem::file_time_type& time) {
  if (time == std::filesystem::file_time_type{}) {
    return i18n::tr("time.file.unknown");
  }
  const auto systemNow = std::chrono::system_clock::now();
  const auto fileNow = std::filesystem::file_time_type::clock::now();
  const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(time - fileNow + systemNow);
  const std::time_t value = std::chrono::system_clock::to_time_t(systemTime);
  std::tm tm{};
  localtime_r(&value, &tm);
  const std::string formatted = formatStrftime("%Y-%m-%d %H:%M", tm);
  if (formatted.empty()) {
    return i18n::tr("time.file.unknown");
  }
  return formatted;
}

std::string formatTimeAgo(std::chrono::system_clock::time_point tp) {
  using namespace std::chrono;
  const auto secs = duration_cast<seconds>(system_clock::now() - tp).count();
  return formatAgeSeconds(secs, tp);
}

std::string formatElapsedSince(std::chrono::steady_clock::time_point since) {
  using namespace std::chrono;
  const auto secs = duration_cast<seconds>(steady_clock::now() - since).count();
  return formatAgeSeconds(secs, std::nullopt);
}

std::string formatDuration(std::chrono::seconds duration) {
  const auto totalSeconds = static_cast<std::uint64_t>(duration.count());
  const std::uint64_t days = totalSeconds / 86400;
  std::uint64_t rem = totalSeconds % 86400;
  const std::uint64_t hours = rem / 3600;
  rem %= 3600;
  const std::uint64_t minutes = rem / 60;
  if (days > 0) {
    const std::string dayText = formatDurationUnit("time.units.day", days);
    if (hours > 0 && minutes > 0) {
      return formatDurationParts(
          dayText, formatDurationUnit("time.units.hour", hours), formatDurationUnit("time.units.minute", minutes)
      );
    }
    if (hours > 0) {
      return formatDurationParts(dayText, formatDurationUnit("time.units.hour", hours));
    }
    if (minutes > 0) {
      return formatDurationParts(dayText, formatDurationUnit("time.units.minute", minutes));
    }
    return dayText;
  }
  if (hours > 0) {
    const std::string hourText = formatDurationUnit("time.units.hour", hours);
    if (minutes > 0) {
      return formatDurationParts(hourText, formatDurationUnit("time.units.minute", minutes));
    }
    return hourText;
  }
  if (minutes > 0) {
    return formatDurationUnit("time.units.minute", minutes);
  }
  return i18n::tr("time.duration.less-than-minute");
}
