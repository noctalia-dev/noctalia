#include "system/icon_resolver.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "icon_resolver_test: {}", message);
    }
    return condition;
  }

} // namespace

int main() {
  char tempDir[] = "/tmp/noctalia-icon-resolver-test-XXXXXX";
  if (mkdtemp(tempDir) == nullptr) {
    std::perror("mkdtemp");
    return 1;
  }

  const fs::path root(tempDir);
  const fs::path iconDir = root / "icons/hicolor/scalable/apps";
  const fs::path deniedDataHome = root / "denied";
  const fs::path deniedIcon = deniedDataHome / "private-icon.svg";
  fs::create_directories(iconDir);
  fs::create_directories(deniedDataHome);
  std::ofstream(deniedIcon) << "<svg/>";
  fs::permissions(deniedDataHome, fs::perms::none);
  setenv("HOME", tempDir, 1);
  setenv("XDG_DATA_HOME", deniedDataHome.c_str(), 1);
  setenv("XDG_DATA_DIRS", tempDir, 1);

  bool ok = true;

  std::array<int, 2> logPipe{-1, -1};
  if (!expect(::pipe(logPipe.data()) == 0, "failed to create stderr capture pipe")) {
    return 1;
  }
  std::fflush(stderr);
  const int savedStderr = ::dup(STDERR_FILENO);
  if (!expect(savedStderr >= 0 && ::dup2(logPipe[1], STDERR_FILENO) >= 0, "failed to redirect stderr")) {
    ::close(logPipe[0]);
    ::close(logPipe[1]);
    if (savedStderr >= 0) {
      ::close(savedStderr);
    }
    return 1;
  }

  IconResolver resolver(true);

  std::fflush(stderr);
  (void)::dup2(savedStderr, STDERR_FILENO);
  ::close(savedStderr);
  ::close(logPipe[1]);

  std::string startupLogs;
  std::array<char, 1024> logBuffer{};
  ssize_t count = 0;
  while ((count = ::read(logPipe[0], logBuffer.data(), logBuffer.size())) > 0) {
    startupLogs.append(logBuffer.data(), static_cast<std::size_t>(count));
  }
  ::close(logPipe[0]);

  const bool permissionsEnforced = (geteuid() != 0);
  if (permissionsEnforced) {
    ok = expect(startupLogs.contains(deniedDataHome.string()), "an inaccessible icon directory should be logged") && ok;
    ok = expect(
             resolver.resolve(deniedIcon.string(), 32).empty(),
             "an icon beneath an inaccessible directory should not terminate resolution"
         )
        && ok;
    fs::permissions(deniedDataHome, fs::perms::owner_all);
  }
  ok =
      expect(!startupLogs.contains((root / ".icons/hicolor").string()), "a missing icon directory should not be logged")
      && ok;
  ok = expect(
           resolver.resolve(deniedIcon.string(), 32) == deniedIcon.string(),
           "an absolute icon denied earlier should resolve once it is readable"
       )
      && ok;

  const fs::path invalidatedIcon = iconDir / "invalidated-icon.svg";
  ok = expect(resolver.resolve("invalidated-icon", 32).empty(), "initial missing icon should not resolve") && ok;
  std::ofstream(invalidatedIcon) << "<svg/>";
  ok = expect(resolver.resolve("invalidated-icon", 32).empty(), "named icon miss should be cached") && ok;
  resolver.invalidateMissingCache();
  ok = expect(
           resolver.resolve("invalidated-icon", 32) == invalidatedIcon.string(),
           "invalidating misses should discover a newly created icon"
       )
      && ok;

  const fs::path polledIcon = iconDir / "polled-icon.svg";
  ok = expect(resolver.resolve("polled-icon", 32).empty(), "second initial icon miss should be cached") && ok;
  std::ofstream(polledIcon) << "<svg/>";
  ok = expect(IconResolver::checkThemeChanged(), "theme poll should detect icon directory changes") && ok;
  ok = expect(
           resolver.resolve("polled-icon", 32) == polledIcon.string(),
           "theme generation change should invalidate cached misses"
       )
      && ok;

  const fs::path absoluteIcon = root / "absolute-icon.svg";
  ok = expect(resolver.resolve(absoluteIcon.string(), 32).empty(), "missing absolute icon should not resolve") && ok;
  std::ofstream(absoluteIcon) << "<svg/>";
  ok = expect(
           resolver.resolve(absoluteIcon.string(), 32) == absoluteIcon.string(),
           "absolute icon misses should not be cached"
       )
      && ok;

  std::error_code ec;
  fs::remove_all(root, ec);
  return ok ? 0 : 1;
}
