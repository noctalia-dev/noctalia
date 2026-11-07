#include "system/desktop_entry.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

  namespace fs = std::filesystem;

  void writeEntry(const fs::path& path, std::string_view name) {
    std::ofstream entry(path);
    entry << "[Desktop Entry]\nType=Application\nName=" << name << "\nExec=true\n";
  }

  // inotify queues an event inside the syscall that causes it, so pending events can be checked without waiting.
  bool watchEventPending() {
    pollfd pfd{.fd = desktopEntryWatchFd(), .events = POLLIN, .revents = 0};
    return poll(&pfd, 1, 0) > 0;
  }

  bool hasId(const std::vector<DesktopEntry>& entries, std::string_view id) {
    for (const auto& entry : entries) {
      if (entry.id == id) {
        return true;
      }
    }
    return false;
  }

} // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / ("noctalia-desktop-entry-watch-" + std::to_string(getpid()));
  const fs::path applications = root / "data/applications";
  fs::create_directories(applications / "nested");
  writeEntry(applications / "watch-probe.desktop", "Watch Probe");

  setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
  setenv("XDG_DATA_DIRS", (root / "empty").c_str(), 1);

  TEST_CHECK(hasId(desktopEntries(), "watch-probe"));

  // A refresh must queue nothing for directories it keeps watching: past the queue limit, that overflows into a rescan.
  writeEntry(applications / "nested/watch-probe-added.desktop", "Added Probe");
  TEST_CHECK(watchEventPending());
  checkDesktopEntryReload();
  TEST_CHECK(hasId(desktopEntries(), "watch-probe-added"));
  TEST_CHECK(!watchEventPending());

  writeEntry(applications / "nested/watch-probe-later.desktop", "Later Probe");
  TEST_CHECK(watchEventPending());

  std::error_code ec;
  fs::remove_all(root, ec);
  return 0;
}
