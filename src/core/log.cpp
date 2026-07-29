#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>

namespace {

  LogLevel gMinLevel = LogLevel::Info;
  FILE* gLogFile = nullptr;
  std::mutex gLogMutex;
  std::string gLogPath;
  std::string gBackupLogPath;
  std::uintmax_t gLogSizeBytes = 0;
  std::size_t gBufferedFileLogLines = 0;
  std::chrono::steady_clock::time_point gLastFileFlushAt = std::chrono::steady_clock::now();
  bool gRegisteredExitFlush = false;

  constexpr std::size_t kMaxLogBytes = 1 * 1024 * 1024; // 1 MB
  constexpr std::size_t kMaxLogLineBytes = 8 * 1024;    // 8 KiB
  constexpr std::size_t kBufferedFileLogFlushLines = 64;
  constexpr auto kBufferedFileLogFlushInterval = std::chrono::milliseconds(500);
  constexpr const char* kLogLevelEnv = "NOCTALIA_LOG_LEVEL";

  struct CappedLogMessage {
    std::string storage;
    std::string_view original;
    bool capped = false;

    [[nodiscard]] std::string_view text() const noexcept { return capped ? std::string_view(storage) : original; }
  };

  const char* levelTagAnsi(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
      return "\033[36mDBG\033[0m";
    case LogLevel::Info:
      return "\033[32mINF\033[0m";
    case LogLevel::Warn:
      return "\033[33mWRN\033[0m";
    case LogLevel::Error:
      return "\033[31mERR\033[0m";
    }
    return "???";
  }

  const char* levelTagPlain(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
      return "DBG";
    case LogLevel::Info:
      return "INF";
    case LogLevel::Warn:
      return "WRN";
    case LogLevel::Error:
      return "ERR";
    }
    return "???";
  }

  std::size_t utf8PrefixBoundary(std::string_view text, std::size_t maxBytes) {
    if (maxBytes >= text.size()) {
      return text.size();
    }

    std::size_t end = maxBytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
      --end;
    }
    return end;
  }

  std::string truncationSuffix(std::size_t originalBytes) {
    return std::string(" ... [truncated, original=") + std::to_string(originalBytes) + " bytes]";
  }

  CappedLogMessage capMessageForLine(std::string_view msg, std::size_t prefixBytes) {
    CappedLogMessage result;
    result.original = msg;

    if (prefixBytes + 1 >= kMaxLogLineBytes) {
      result.capped = true;
      return result;
    }

    const std::size_t maxMessageBytes = kMaxLogLineBytes - prefixBytes - 1;
    if (msg.size() <= maxMessageBytes) {
      return result;
    }

    result.capped = true;

    std::string suffix = truncationSuffix(msg.size());
    if (suffix.size() > maxMessageBytes) {
      constexpr std::string_view kShortSuffix = " ... [truncated]";
      result.storage = std::string(kShortSuffix.substr(0, std::min(kShortSuffix.size(), maxMessageBytes)));
      return result;
    }

    const std::size_t bodyBytes = utf8PrefixBoundary(msg, maxMessageBytes - suffix.size());
    result.storage.reserve(bodyBytes + suffix.size());
    result.storage.append(msg.substr(0, bodyBytes));
    result.storage.append(suffix);
    return result;
  }

  std::size_t formattedPrefixLength(int length, std::size_t bufferSize) {
    if (length <= 0 || bufferSize == 0) {
      return 0;
    }
    return std::min(static_cast<std::size_t>(length), bufferSize - 1);
  }

  std::string consolePrefix(const std::tm& tm, const std::timespec& ts, LogLevel level, const char* section) {
    char buffer[96];
    const int length = std::snprintf(
        buffer, sizeof(buffer), "%02d:%02d:%02d.%03ld [%s]", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1'000'000,
        levelTagAnsi(level)
    );

    std::string prefix(buffer, formattedPrefixLength(length, sizeof(buffer)));
    if (section != nullptr && section[0] != '\0') {
      prefix += " [\033[34m";
      prefix += section;
      prefix += "\033[0m]";
    }
    prefix += ' ';
    return prefix;
  }

  std::string filePrefix(const std::tm& tm, const std::timespec& ts, LogLevel level, const char* section) {
    char buffer[128];
    const int length = std::snprintf(
        buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03ld [%s]", tm.tm_year + 1900, tm.tm_mon + 1,
        tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1'000'000, levelTagPlain(level)
    );

    std::string prefix(buffer, formattedPrefixLength(length, sizeof(buffer)));
    if (section != nullptr && section[0] != '\0') {
      prefix += " [";
      prefix += section;
      prefix += ']';
    }
    prefix += ' ';
    return prefix;
  }

  std::size_t writeLine(FILE* stream, std::string_view prefix, std::string_view msg) {
    if (stream == nullptr) {
      return 0;
    }

    std::size_t bytes = 0;
    if (!prefix.empty()) {
      bytes += std::fwrite(prefix.data(), 1, prefix.size(), stream);
    }
    if (!msg.empty()) {
      bytes += std::fwrite(msg.data(), 1, msg.size(), stream);
    }
    if (std::fputc('\n', stream) != EOF) {
      ++bytes;
    }
    return bytes;
  }

  void flushLogFileUnlocked() {
    if (gLogFile == nullptr) {
      return;
    }
    std::fflush(gLogFile);
    gBufferedFileLogLines = 0;
    gLastFileFlushAt = std::chrono::steady_clock::now();
  }

  void flushLogFileAtExit() {
    std::scoped_lock lock(gLogMutex);
    flushLogFileUnlocked();
  }

  bool shouldFlushLogFile(LogLevel level) {
    if (level >= LogLevel::Warn) {
      return true;
    }

    ++gBufferedFileLogLines;
    const auto now = std::chrono::steady_clock::now();
    return gBufferedFileLogLines >= kBufferedFileLogFlushLines
        || now - gLastFileFlushAt >= kBufferedFileLogFlushInterval;
  }

  void closeLogFileUnlocked() {
    if (gLogFile == nullptr) {
      return;
    }

    std::fflush(gLogFile);
    std::fclose(gLogFile);
    gLogFile = nullptr;
  }

  std::uintmax_t currentLogFileSizeUnlocked() {
    if (gLogPath.empty()) {
      return 0;
    }

    std::error_code ec;
    const auto size = std::filesystem::file_size(gLogPath, ec);
    return ec ? 0 : size;
  }

  void openLogFileUnlocked() {
    if (gLogPath.empty()) {
      return;
    }

    gLogFile = std::fopen(gLogPath.c_str(), "a");
    gLogSizeBytes = gLogFile == nullptr ? 0 : currentLogFileSizeUnlocked();
    gBufferedFileLogLines = 0;
    gLastFileFlushAt = std::chrono::steady_clock::now();
  }

  void rotateLogFileUnlocked() {
    closeLogFileUnlocked();

    if (gLogPath.empty() || gBackupLogPath.empty()) {
      return;
    }

    std::error_code ec;
    std::filesystem::remove(gBackupLogPath, ec);
    ec.clear();
    std::filesystem::rename(gLogPath, gBackupLogPath, ec);
    openLogFileUnlocked();
  }

} // namespace

std::string_view logLevelName(LogLevel level) {
  switch (level) {
  case LogLevel::Debug:
    return "debug";
  case LogLevel::Info:
    return "info";
  case LogLevel::Warn:
    return "warn";
  case LogLevel::Error:
    return "error";
  }
  return "unknown";
}

std::optional<LogLevel> parseLogLevel(std::string_view value) {
  if (value == "debug") {
    return LogLevel::Debug;
  }
  if (value == "info") {
    return LogLevel::Info;
  }
  if (value == "warn") {
    return LogLevel::Warn;
  }
  if (value == "error") {
    return LogLevel::Error;
  }
  return std::nullopt;
}

void initLogFile() {
  const char* cacheHome = std::getenv("XDG_CACHE_HOME");
  const char* home = std::getenv("HOME");

  std::string dir;
  if (cacheHome != nullptr && cacheHome[0] != '\0') {
    dir = std::string(cacheHome) + "/noctalia";
  } else if (home != nullptr && home[0] != '\0') {
    dir = std::string(home) + "/.cache/noctalia";
  } else {
    return; // no writable location available
  }

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return;
  }

  const std::string logPath = dir + "/noctalia.log";
  const std::string backupPath = dir + "/noctalia.log.1";

  std::scoped_lock lock(gLogMutex);
  closeLogFileUnlocked();
  gLogPath = logPath;
  gBackupLogPath = backupPath;

  const auto size = std::filesystem::file_size(gLogPath, ec);
  if (!ec && size > kMaxLogBytes) {
    rotateLogFileUnlocked();
  } else {
    openLogFileUnlocked();
  }

  if (gLogFile != nullptr && !gRegisteredExitFlush) {
    (void)std::atexit(flushLogFileAtExit);
    gRegisteredExitFlush = true;
  }
}

namespace detail {

  void logMessage(LogLevel level, const char* section, std::string_view msg) {
    std::timespec ts{};
    std::timespec_get(&ts, TIME_UTC);
    std::tm tm{};
    localtime_r(&ts.tv_sec, &tm);

    std::scoped_lock lock(gLogMutex);

    // Console: respects gMinLevel, ANSI colours, time only
    if (level >= gMinLevel) {
      const std::string prefix = consolePrefix(tm, ts, level, section);
      const CappedLogMessage capped = capMessageForLine(msg, prefix.size());
      (void)writeLine(stderr, prefix, capped.text());
    }

    // File: always unfiltered, no ANSI, full date for context
    if (gLogFile != nullptr) {
      const std::string prefix = filePrefix(tm, ts, level, section);
      const CappedLogMessage capped = capMessageForLine(msg, prefix.size());
      const std::string_view cappedText = capped.text();
      const std::uintmax_t lineBytes = prefix.size() + cappedText.size() + 1;
      if (gLogSizeBytes > 0 && gLogSizeBytes + lineBytes > kMaxLogBytes) {
        rotateLogFileUnlocked();
      }
      gLogSizeBytes += writeLine(gLogFile, prefix, cappedText);
      if (shouldFlushLogFile(level)) {
        flushLogFileUnlocked();
      }
    }
  }

} // namespace detail

LogLevel currentLogLevel() {
  std::scoped_lock lock(gLogMutex);
  return gMinLevel;
}

void setLogLevel(LogLevel level) {
  std::scoped_lock lock(gLogMutex);
  gMinLevel = level;
}

void initLogLevelFromEnvironment() {
  const char* value = std::getenv(kLogLevelEnv);
  if (value == nullptr) {
    return;
  }

  const auto level = parseLogLevel(value);
  if (!level.has_value()) {
    logWarn("invalid {} '{}'; expected debug, info, warn, or error", kLogLevelEnv, value);
    return;
  }

  setLogLevel(*level);
  logInfo("log level set to {}", logLevelName(*level));
}
