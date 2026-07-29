#include "core/log.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <print>
#include <string>
#include <unistd.h>

namespace {

  constexpr std::size_t kMaxLogBytes = 1 * 1024 * 1024;
  constexpr std::size_t kMaxLogLineBytes = 8 * 1024;

  std::filesystem::path makeTempRoot(const char* label) {
    static int counter = 0;
    auto path = std::filesystem::temp_directory_path()
        / (std::string(label) + "-" + std::to_string(getpid()) + "-" + std::to_string(counter++));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
  }

  bool useCacheHome(const std::filesystem::path& path) { return setenv("XDG_CACHE_HOME", path.c_str(), 1) == 0; }

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "log_test: {}", message);
    }
    return condition;
  }

  bool parsesLogLevels() {
    bool ok = true;
    ok = expect(parseLogLevel("debug") == LogLevel::Debug, "debug log level did not parse") && ok;
    ok = expect(parseLogLevel("info") == LogLevel::Info, "info log level did not parse") && ok;
    ok = expect(parseLogLevel("warn") == LogLevel::Warn, "warn log level did not parse") && ok;
    ok = expect(parseLogLevel("error") == LogLevel::Error, "error log level did not parse") && ok;
    ok = expect(!parseLogLevel("WRN").has_value(), "non-canonical log level was accepted") && ok;
    ok = expect(logLevelName(LogLevel::Warn) == "warn", "warn log level name was wrong") && ok;
    return ok;
  }

  bool readsLogLevelEnvironment() {
    setLogLevel(LogLevel::Info);
    bool ok = expect(setenv("NOCTALIA_LOG_LEVEL", "warn", 1) == 0, "failed to set NOCTALIA_LOG_LEVEL");
    initLogLevelFromEnvironment();
    ok = expect(currentLogLevel() == LogLevel::Warn, "environment log level was not applied") && ok;

    setLogLevel(LogLevel::Error);
    ok = expect(setenv("NOCTALIA_LOG_LEVEL", "WRN", 1) == 0, "failed to set invalid NOCTALIA_LOG_LEVEL") && ok;
    initLogLevelFromEnvironment();
    ok = expect(currentLogLevel() == LogLevel::Error, "invalid environment log level changed current level") && ok;
    unsetenv("NOCTALIA_LOG_LEVEL");
    return ok;
  }

  bool writesCappedLogLines() {
    const auto cacheRoot = makeTempRoot("noctalia-log-cap");
    if (!expect(useCacheHome(cacheRoot), "failed to set XDG_CACHE_HOME")) {
      return false;
    }

    initLogFile();
    logWarn("{}", std::string(10'000, 'x'));

    const auto logPath = cacheRoot / "noctalia" / "noctalia.log";
    std::error_code ec;
    const auto size = std::filesystem::file_size(logPath, ec);
    bool ok = expect(!ec, "failed to stat capped log file");
    ok = expect(size <= kMaxLogLineBytes, "capped log line exceeded 8 KiB") && ok;

    const std::string log = readFile(logPath);
    ok = expect(log.contains("truncated, original=10000 bytes"), "missing truncation marker") && ok;

    std::filesystem::remove_all(cacheRoot);
    return ok;
  }

  bool rotatesWhileRunning() {
    const auto cacheRoot = makeTempRoot("noctalia-log-rotate");
    if (!expect(useCacheHome(cacheRoot), "failed to set XDG_CACHE_HOME")) {
      return false;
    }

    const auto logDir = cacheRoot / "noctalia";
    std::filesystem::create_directories(logDir);

    const auto logPath = logDir / "noctalia.log";
    const std::size_t initialSize = kMaxLogBytes - 8;
    {
      std::ofstream out(logPath, std::ios::binary);
      const std::string chunk(4096, 'a');
      std::size_t written = 0;
      while (written + chunk.size() <= initialSize) {
        out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        written += chunk.size();
      }
      const std::string tail(initialSize - written, 'a');
      out.write(tail.data(), static_cast<std::streamsize>(tail.size()));
    }

    initLogFile();
    logWarn("rotate-now");

    const auto backupPath = logDir / "noctalia.log.1";
    std::error_code ec;
    const auto backupSize = std::filesystem::file_size(backupPath, ec);
    bool ok = expect(!ec, "missing rotated backup");
    ok = expect(backupSize == initialSize, "rotated backup size did not match original log") && ok;

    const std::string currentLog = readFile(logPath);
    ok = expect(currentLog.contains("rotate-now"), "new log did not receive post-rotation line") && ok;
    ok = expect(currentLog.size() <= kMaxLogLineBytes, "post-rotation log line exceeded 8 KiB") && ok;

    std::filesystem::remove_all(cacheRoot);
    return ok;
  }

} // namespace

int main() {
  setLogLevel(LogLevel::Error);

  bool ok = true;
  ok = parsesLogLevels() && ok;
  ok = readsLogLevelEnvironment() && ok;
  ok = writesCappedLogLines() && ok;
  ok = rotatesWhileRunning() && ok;
  return ok ? 0 : 1;
}
