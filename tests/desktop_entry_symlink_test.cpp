#include "system/desktop_entry.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
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

} // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / ("noctalia-desktop-entry-symlink-" + std::to_string(getpid()));
  const fs::path applications = root / "data/applications";
  const fs::path elsewhere = root / "elsewhere";
  fs::create_directories(applications / "nested");
  fs::create_directories(elsewhere);

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

  std::error_code ec;
  fs::remove_all(root, ec);
  return 0;
}
