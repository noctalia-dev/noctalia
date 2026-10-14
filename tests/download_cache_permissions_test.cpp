#include "test_check.h"
#include "util/file_utils.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  std::filesystem::perms mode(const std::filesystem::path& path) {
    return std::filesystem::status(path).permissions() & std::filesystem::perms::all;
  }
} // namespace

int main() {
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() / ("noctalia-download-cache-test-" + std::to_string(::getpid()));
  fs::create_directories(root);
  TEST_CHECK(::setenv("HOME", root.c_str(), 1) == 0);
  TEST_CHECK(::unsetenv("XDG_CACHE_HOME") == 0);
  const mode_t oldUmask = ::umask(0022);

  const auto fallback = root / ".cache/noctalia/downloads";
  TEST_CHECK(FileUtils::downloadCacheDir("location") == fallback / "location");
  TEST_CHECK(mode(fallback.parent_path()) == FileUtils::privateDirectoryMode());
  TEST_CHECK(mode(fallback) == FileUtils::privateDirectoryMode());
  TEST_CHECK(mode(fallback / "location") == FileUtils::privateDirectoryMode());

  const auto base = root / "configured-cache";
  TEST_CHECK(::setenv("XDG_CACHE_HOME", base.c_str(), 1) == 0);
  const auto cache = base / "noctalia/downloads";
  fs::create_directories(cache / "weather");
  std::ofstream(cache / "weather/forecast.json") << "private coordinates";
  for (const auto& directory : {cache.parent_path(), cache, cache / "weather"}) {
    fs::permissions(directory, static_cast<fs::perms>(0755));
  }
  TEST_CHECK(FileUtils::downloadCacheDir("weather") == cache / "weather");
  TEST_CHECK(mode(base) == static_cast<fs::perms>(0755));
  for (const auto& directory : {cache.parent_path(), cache, cache / "weather"}) {
    TEST_CHECK(mode(directory) == FileUtils::privateDirectoryMode());
  }
  TEST_CHECK(fs::file_size(cache / "weather/forecast.json") > 0);

  const auto target = root / "symlink-target";
  fs::create_directory(target);
  fs::permissions(target, static_cast<fs::perms>(0755));
  fs::remove_all(cache / "weather");
  fs::create_directory_symlink(target, cache / "weather");
  TEST_CHECK(FileUtils::downloadCacheDir("weather").empty());
  TEST_CHECK(mode(target) == static_cast<fs::perms>(0755));
  fs::remove_all(cache);
  fs::create_directory_symlink(target, cache);
  TEST_CHECK(FileUtils::downloadCacheDir().empty());
  TEST_CHECK(mode(target) == static_cast<fs::perms>(0755));
  fs::remove_all(base / "noctalia");
  fs::create_directory_symlink(target, base / "noctalia");
  TEST_CHECK(FileUtils::downloadCacheDir().empty());
  TEST_CHECK(mode(target) == static_cast<fs::perms>(0755));

  fs::remove(base / "noctalia");
  std::ofstream(base / "noctalia") << "not a directory";
  TEST_CHECK(FileUtils::downloadCacheDir().empty());
  fs::remove(base / "noctalia");
  TEST_CHECK(::mkfifo((base / "noctalia").c_str(), 0600) == 0);
  TEST_CHECK(FileUtils::downloadCacheDir().empty());
  fs::remove(base / "noctalia");
  fs::permissions(base, static_cast<fs::perms>(0777));
  TEST_CHECK(FileUtils::downloadCacheDir().empty());
  TEST_CHECK(!fs::exists(base / "noctalia"));
  fs::permissions(base, static_cast<fs::perms>(0755));
  TEST_CHECK(FileUtils::downloadCacheDir("../outside").empty());
  TEST_CHECK(FileUtils::downloadCacheDir("..").empty());
  TEST_CHECK(!fs::exists(base / "noctalia"));

  TEST_CHECK(::setenv("XDG_CACHE_HOME", "relative", 1) == 0);
  TEST_CHECK(FileUtils::downloadCacheDir() == fallback);
  TEST_CHECK(::unsetenv("HOME") == 0);
  TEST_CHECK(FileUtils::downloadCacheDir().empty());
  TEST_CHECK(::setenv("HOME", "relative", 1) == 0);
  TEST_CHECK(FileUtils::downloadCacheDir().empty());
  TEST_CHECK(::setenv("XDG_CACHE_HOME", base.c_str(), 1) == 0);
  TEST_CHECK(FileUtils::downloadCacheDir() == cache);
  const auto linkedBase = root / "linked-cache";
  fs::create_directory_symlink(base, linkedBase);
  TEST_CHECK(::setenv("XDG_CACHE_HOME", linkedBase.c_str(), 1) == 0);
  TEST_CHECK(FileUtils::downloadCacheDir("weather") == linkedBase / "noctalia/downloads/weather");

  ::umask(oldUmask);
  fs::remove_all(root);
}
