#include "shell/wallpaper/panel/wallpaper_scanner.h"
#include "tests/test_check.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <string>
#include <vector>

namespace {

  namespace fs = std::filesystem;

  void touch(const fs::path& path) {
    std::ofstream out(path);
    out << "x";
  }

  std::size_t countNamed(const std::vector<WallpaperEntry>& entries, std::string_view name) {
    return static_cast<std::size_t>(std::ranges::count_if(entries, [name](const WallpaperEntry& e) {
      return e.name == name;
    }));
  }

  // Runs the scan to completion through the poll loop, the same way the shell
  // drives the scanner. Returns nullptr if no result lands before the deadline.
  const WallpaperScanResult* scanBlocking(WallpaperScanner& scanner, const fs::path& dir) {
    if (scanner.requestScan(dir, true)) {
      return scanner.cached(dir, true);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
      std::vector<pollfd> fds;
      const std::size_t start = scanner.addPollFds(fds);
      if (fds.empty()) {
        return nullptr;
      }
      if (::poll(fds.data(), fds.size(), 200) < 0) {
        return nullptr;
      }
      scanner.dispatch(fds, start);
      if (const auto* result = scanner.cached(dir, true)) {
        return result;
      }
    }
    return nullptr;
  }

} // namespace

int main() {
  const fs::path tempDir = fs::temp_directory_path()
      / ("noctalia-wallpaper-scan-symlink-"
         + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const fs::path root = tempDir / "root";
  const fs::path local = root / "local";
  const fs::path external = tempDir / "external";
  fs::create_directories(local);
  fs::create_directories(external / "nested");

  touch(local / "local.png");
  touch(external / "external.png");
  touch(external / "nested" / "nested.png");

  // The reported setup: a directory symlink sitting next to a real one.
  fs::create_directory_symlink(external, root / "linked");
  // A symlink pointing back at the scan root, which loops without a guard.
  fs::create_directory_symlink(root, external / "nested" / "loop");
  // A second symlink to the same target, which duplicates without a guard.
  fs::create_directory_symlink(external, root / "linked-again");

  // Read everything out before removing the temp dir, so a failed check does
  // not leave it behind.
  bool completed = false;
  std::size_t localCount = 0;
  std::size_t externalCount = 0;
  std::size_t nestedCount = 0;
  std::size_t entryCount = 0;
  {
    WallpaperScanner scanner;
    const WallpaperScanResult* result = scanBlocking(scanner, root);
    completed = result != nullptr;
    if (completed) {
      localCount = countNamed(result->entries, "local.png");
      externalCount = countNamed(result->entries, "external.png");
      nestedCount = countNamed(result->entries, "nested.png");
      entryCount = result->entries.size();
    }
  }

  std::error_code ec;
  fs::remove_all(tempDir, ec);

  // Flatten scan did not complete.
  TEST_CHECK(completed);
  // Local image missing or duplicated.
  TEST_CHECK(localCount == 1);
  // Symlinked image missing or duplicated.
  TEST_CHECK(externalCount == 1);
  // Image under symlinked subdirectory missing.
  TEST_CHECK(nestedCount == 1);
  TEST_CHECK(entryCount == 3);
  return 0;
}
