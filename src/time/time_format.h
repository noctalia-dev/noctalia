#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>

std::string formatTimeAgo(std::chrono::system_clock::time_point tp);

// Same wording as formatTimeAgo, but duration is computed from steady_clock (e.g. Notification::receivedTime).
[[nodiscard]] std::string formatElapsedSince(std::chrono::steady_clock::time_point since);

// Formats a duration using translated day/hour/minute units.
[[nodiscard]] std::string formatDuration(std::chrono::seconds duration);

// Formats seconds as clock-style "M:SS" or "H:MM:SS". Returns "0:00" for <= 0.
[[nodiscard]] std::string formatClockTime(std::int64_t seconds);

// Formats the current local date using the locale's preferred format.
[[nodiscard]] std::string formatCurrentDate();

// First day of the week from the active LC_TIME locale (struct tm::tm_wday encoding: Sun=0 .. Sat=6).
[[nodiscard]] int localeFirstDayOfWeek();

// Formats current local time with a C++20 chrono format string (e.g. "{:%H:%M}").
// Bare chrono specs such as "%H:%M" are accepted, as are strftime-style no-pad
// numeric specifiers such as "%-I".
[[nodiscard]] std::string formatLocalTime(const char* fmt);

// Formats a Unix timestamp in local time. Accepts the same date-format syntax as formatLocalTime(),
// and handles strftime "%s" as Unix epoch seconds in Noctalia-owned code.
[[nodiscard]] std::string formatLocalUnixTime(std::int64_t unixSeconds, std::string_view fmt);

// Formats an ISO 8601 time string (e.g. "2026-05-09T06:23") using the given format.
[[nodiscard]] std::string formatIsoTime(std::string_view isoTime, const char* fmt);

// Formats a std::tm with strftime semantics using a dynamically sized buffer.
[[nodiscard]] std::string formatStrftime(std::string_view fmt, const std::tm& tm);

// Formats a time_point as UTC (gmtime) with strftime semantics, e.g. "%Y-%m-%dT%H:%M:%SZ".
[[nodiscard]] std::string formatUtcTime(std::chrono::system_clock::time_point tp, std::string_view fmt);

// Formats a filesystem modification time as "YYYY-MM-DD HH:MM".
[[nodiscard]] std::string formatFileTime(const std::filesystem::file_time_type& time);
