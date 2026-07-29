#include "system/icon_resolver.h"

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
  fs::create_directories(iconDir);
  setenv("HOME", tempDir, 1);
  setenv("XDG_DATA_HOME", tempDir, 1);
  setenv("XDG_DATA_DIRS", tempDir, 1);

  bool ok = true;
  IconResolver resolver(true);

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
