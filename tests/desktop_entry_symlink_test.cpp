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

  std::size_t countId(const std::vector<DesktopEntry>& entries, std::string_view id) {
    std::size_t count = 0;
    for (const auto& entry : entries) {
      if (entry.id == id) {
        ++count;
      }
    }
    return count;
  }

  std::string nameOf(const std::vector<DesktopEntry>& entries, std::string_view id) {
    for (const auto& entry : entries) {
      if (entry.id == id) {
        return entry.name;
      }
    }
    return {};
  }

  bool waitForWatchEvent() {
    pollfd pfd{.fd = desktopEntryWatchFd(), .events = POLLIN, .revents = 0};
    if (poll(&pfd, 1, 2000) <= 0) {
      return false;
    }
    checkDesktopEntryReload();
    return true;
  }

} // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / ("noctalia-desktop-entry-symlink-" + std::to_string(getpid()));
  const fs::path applications = root / "data/applications";
  const fs::path elsewhere = root / "elsewhere";
  fs::create_directories(applications / "nested");
  fs::create_directories(elsewhere / "deeper");

  writeEntry(applications / "symlink-probe-plain.desktop", "Plain Probe");
  writeEntry(applications / "nested/symlink-probe-nested.desktop", "Nested Probe");
  writeEntry(elsewhere / "symlink-probe-linked.desktop", "Linked Probe");

  fs::create_directory_symlink(elsewhere, applications / "linked");
  fs::create_directory_symlink(elsewhere, applications / "linked-again");
  fs::create_directory_symlink(applications, applications / "cycle");
  fs::create_directory_symlink(applications / "missing", applications / "broken");

  setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
  setenv("XDG_DATA_DIRS", (root / "empty").c_str(), 1);

  const auto entries = scanDesktopEntries();

  TEST_CHECK(countId(entries, "symlink-probe-plain") == 1);
  TEST_CHECK(countId(entries, "symlink-probe-nested") == 1);
  TEST_CHECK(countId(entries, "symlink-probe-linked") == 1);

  const auto initialVersion = desktopEntriesVersion();

  writeEntry(elsewhere / "symlink-probe-added.desktop", "Added Probe");
  TEST_CHECK(waitForWatchEvent());
  const auto addedVersion = desktopEntriesVersion();
  TEST_CHECK(addedVersion > initialVersion);
  TEST_CHECK(countId(desktopEntries(), "symlink-probe-added") == 1);

  writeEntry(elsewhere / "deeper/symlink-probe-deeper.desktop", "Deeper Probe");
  TEST_CHECK(waitForWatchEvent());
  const auto deeperVersion = desktopEntriesVersion();
  TEST_CHECK(deeperVersion > addedVersion);
  TEST_CHECK(countId(desktopEntries(), "symlink-probe-deeper") == 1);

  writeEntry(elsewhere / "deeper/symlink-probe-deeper.desktop", "Renamed Probe");
  TEST_CHECK(waitForWatchEvent());
  TEST_CHECK(desktopEntriesVersion() > deeperVersion);
  TEST_CHECK(nameOf(desktopEntries(), "symlink-probe-deeper") == "Renamed Probe");

  std::error_code ec;
  fs::remove_all(root, ec);
  return 0;
}
