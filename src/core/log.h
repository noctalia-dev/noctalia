#pragma once

#include <format>
#include <optional>
#include <string_view>
#include <utility>

enum class LogLevel { Debug, Info, Warn, Error };

namespace detail {
  void logMessage(LogLevel level, const char* section, std::string_view msg);
} // namespace detail

// ── Global free functions (no section tag) ────────────────────────────────────

template <typename... Args> void logDebug(std::format_string<Args...> fmt, Args&&... args) {
  detail::logMessage(LogLevel::Debug, nullptr, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void logInfo(std::format_string<Args...> fmt, Args&&... args) {
  detail::logMessage(LogLevel::Info, nullptr, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void logWarn(std::format_string<Args...> fmt, Args&&... args) {
  detail::logMessage(LogLevel::Warn, nullptr, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void logError(std::format_string<Args...> fmt, Args&&... args) {
  detail::logMessage(LogLevel::Error, nullptr, std::format(fmt, std::forward<Args>(args)...));
}

// ── Logger — per-module logger with a section tag ─────────────────────────────

class Logger {
public:
  explicit constexpr Logger(const char* section) : m_section(section) {}

  template <typename... Args> void debug(std::format_string<Args...> fmt, Args&&... args) const {
    detail::logMessage(LogLevel::Debug, m_section, std::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args> void info(std::format_string<Args...> fmt, Args&&... args) const {
    detail::logMessage(LogLevel::Info, m_section, std::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args> void warn(std::format_string<Args...> fmt, Args&&... args) const {
    detail::logMessage(LogLevel::Warn, m_section, std::format(fmt, std::forward<Args>(args)...));
  }

  template <typename... Args> void error(std::format_string<Args...> fmt, Args&&... args) const {
    detail::logMessage(LogLevel::Error, m_section, std::format(fmt, std::forward<Args>(args)...));
  }

private:
  const char* m_section;
};

[[nodiscard]] std::string_view logLevelName(LogLevel level);
[[nodiscard]] std::optional<LogLevel> parseLogLevel(std::string_view value);
[[nodiscard]] LogLevel currentLogLevel();
void setLogLevel(LogLevel level);
void initLogLevelFromEnvironment();
void initLogFile();
