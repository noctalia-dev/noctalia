#include "core/log.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

  void parsesLogLevels() {
    TEST_CHECK(parseLogLevel("debug") == LogLevel::Debug);
    TEST_CHECK(parseLogLevel("info") == LogLevel::Info);
    TEST_CHECK(parseLogLevel("warn") == LogLevel::Warn);
    TEST_CHECK(parseLogLevel("error") == LogLevel::Error);
    TEST_CHECK(!parseLogLevel("WRN").has_value());
    TEST_CHECK(logLevelName(LogLevel::Warn) == "warn");
  }

  void readsLogLevelEnvironment() {
    setLogLevel(LogLevel::Info);
    TEST_CHECK(setenv("NOCTALIA_LOG_LEVEL", "warn", 1) == 0);
    initLogLevelFromEnvironment();
    TEST_CHECK(currentLogLevel() == LogLevel::Warn);

    setLogLevel(LogLevel::Error);
    TEST_CHECK(setenv("NOCTALIA_LOG_LEVEL", "WRN", 1) == 0);
    initLogLevelFromEnvironment();
    TEST_CHECK(currentLogLevel() == LogLevel::Error);
    unsetenv("NOCTALIA_LOG_LEVEL");
  }

  void writesCappedLogLines() {
    const auto cacheRoot = makeTempRoot("noctalia-log-cap");
    const bool cacheHomeSet = useCacheHome(cacheRoot);
    if (!cacheHomeSet) {
      std::filesystem::remove_all(cacheRoot);
    }
    TEST_CHECK(cacheHomeSet);

    initLogFile();
    logWarn("{}", std::string(10'000, 'x'));

    const auto logPath = cacheRoot / "noctalia" / "noctalia.log";
    std::error_code ec;
    const auto size = std::filesystem::file_size(logPath, ec);
    const std::string log = readFile(logPath);

    std::filesystem::remove_all(cacheRoot);

    TEST_CHECK(!ec);
    TEST_CHECK(size <= kMaxLogLineBytes);
    TEST_CHECK(log.contains("truncated, original=10000 bytes"));
  }

  void rotatesWhileRunning() {
    const auto cacheRoot = makeTempRoot("noctalia-log-rotate");
    const bool cacheHomeSet = useCacheHome(cacheRoot);
    if (!cacheHomeSet) {
      std::filesystem::remove_all(cacheRoot);
    }
    TEST_CHECK(cacheHomeSet);
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
    const std::string currentLog = readFile(logPath);

    std::filesystem::remove_all(cacheRoot);

    TEST_CHECK(!ec);
    TEST_CHECK(backupSize == initialSize);
    TEST_CHECK(currentLog.contains("rotate-now"));
    TEST_CHECK(currentLog.size() <= kMaxLogLineBytes);
  }

} // namespace

int main() {
  setLogLevel(LogLevel::Error);

  parsesLogLevels();
  readsLogLevelEnvironment();
  writesCappedLogLines();
  rotatesWhileRunning();
  return 0;
}
