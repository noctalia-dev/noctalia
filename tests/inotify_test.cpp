#include "core/inotify/inotify.h"
#include "tests/test_check.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

  std::filesystem::path uniqueTempDir() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path()
        / ("noctalia-inotify-test-" + std::to_string(::getpid()) + "-" + std::to_string(now));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
  }

  void cleanup(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }

  void watchOnValidDirectoryReturnsId() {
    Inotify in;
    const auto dir = uniqueTempDir();
    auto wd = in.watch(dir, IN_CREATE);
    cleanup(dir);
    TEST_CHECK(wd.has_value());
  }

  void watchOnMissingPathReturnsNullopt() {
    Inotify in;
    const auto dir = uniqueTempDir();
    const auto missing = dir / "does-not-exist";
    auto wd = in.watch(missing, IN_CREATE);
    cleanup(dir);
    TEST_CHECK(!wd.has_value());
  }

  void watchOnEmptyPathReturnsNullopt() {
    Inotify in;
    auto wd = in.watch(std::filesystem::path{}, IN_CREATE);
    TEST_CHECK(!wd.has_value());
  }

  void drainWithCallbackFiresOnEvent() {
    Inotify in;
    const auto dir = uniqueTempDir();

    bool fired = false;
    uint32_t mask = 0;
    std::string name;
    in.watch(dir, IN_CREATE);

    const auto file = dir / "child.txt";
    std::ofstream{file} << "hello";

    in.drain([&](const inotify_event* e) {
      fired = true;
      mask = e->mask;
      if (e->len > 0) {
        name = std::string(e->name);
      }
    });

    cleanup(dir);
    TEST_CHECK(fired);
    TEST_CHECK((mask & IN_CREATE) != 0);
    TEST_CHECK(name == "child.txt");
  }

  void drainWithCallbackFiresForAllWatches() {
    Inotify in;
    const auto dir = uniqueTempDir();
    const auto dirA = dir / "a";
    const auto dirB = dir / "b";
    std::error_code ec;
    std::filesystem::create_directories(dirA, ec);
    std::filesystem::create_directories(dirB, ec);

    in.watch(dirA, IN_CREATE);
    in.watch(dirB, IN_CREATE);

    int globalHits = 0;
    in.drain([&](const inotify_event*) { ++globalHits; });
    // No events yet, so nothing should fire.
    const int hitsBeforeEvents = globalHits;

    std::ofstream{dirA / "file_a.txt"} << "x";
    in.drain([&](const inotify_event*) { ++globalHits; });
    const int hitsAfterA = globalHits;

    std::ofstream{dirB / "file_b.txt"} << "y";
    in.drain([&](const inotify_event*) { ++globalHits; });

    cleanup(dir);
    TEST_CHECK(hitsBeforeEvents == 0);
    TEST_CHECK(hitsAfterA == 1);
    TEST_CHECK(globalHits == 2);
  }

  void unwatchSilencesCallbackAndKeepsObjectUsable() {
    Inotify in;
    const auto dir = uniqueTempDir();

    const auto wd = in.watch(dir, IN_CREATE);

    // Drain any events queued while wiring up the watch so the count starts at zero.
    in.drain();

    int fired = 0;
    std::ofstream{dir / "a.txt"} << "1";
    in.drain([&](const inotify_event*) { ++fired; });
    const int firedWhileWatched = fired;

    // After unwatch, the same directory must stop delivering events to the callback.
    if (wd.has_value()) {
      in.unwatch(*wd);
    }
    std::ofstream{dir / "b.txt"} << "2";
    in.drain([&](const inotify_event*) { ++fired; });

    // The object must remain usable: descriptor intact and a fresh watch succeeds.
    const auto wd2 = in.watch(dir, IN_CREATE);
    const int fd = in.fd();

    int firedAgain = 0;
    std::ofstream{dir / "c.txt"} << "3";
    in.drain([&](const inotify_event*) { ++firedAgain; });
    cleanup(dir);

    TEST_CHECK(wd.has_value());
    TEST_CHECK(firedWhileWatched == 1);
    // no callback should fire after unwatch
    TEST_CHECK(fired == 1);
    TEST_CHECK(wd2.has_value());
    TEST_CHECK(fd >= 0);
    TEST_CHECK(firedAgain == 1);
  }

  void queueOverflowIsForwarded() {
    Inotify in;
    const auto dir = uniqueTempDir();
    const auto fileA = dir / "a";
    const auto fileB = dir / "b";
    std::ofstream{fileA};
    std::ofstream{fileB};

    const auto wdA = in.watch(fileA, IN_OPEN);
    const auto wdB = in.watch(fileB, IN_OPEN);

    std::ifstream limitFile("/proc/sys/fs/inotify/max_queued_events");
    std::size_t maxQueuedEvents = 0;
    const bool limitRead = static_cast<bool>(limitFile >> maxQueuedEvents) && maxQueuedEvents > 0;

    // Alternate watch descriptors so adjacent IN_OPEN events are not coalesced.
    bool allOpened = true;
    for (std::size_t i = 0; i <= maxQueuedEvents; ++i) {
      std::ifstream opened{i % 2 == 0 ? fileA : fileB};
      if (!opened.is_open()) {
        allOpened = false;
        break;
      }
    }

    bool overflowForwarded = false;
    int overflowWd = 0;
    in.drain([&](const inotify_event* event) {
      if ((event->mask & IN_Q_OVERFLOW) != 0) {
        overflowForwarded = true;
        overflowWd = event->wd;
      }
    });
    cleanup(dir);

    TEST_CHECK(wdA.has_value() && wdB.has_value());
    // kernel inotify queue limit should be readable
    TEST_CHECK(limitRead);
    TEST_CHECK(allOpened);
    TEST_CHECK(overflowForwarded);
    // IN_Q_OVERFLOW should carry the global watch descriptor
    TEST_CHECK(overflowWd == -1);
  }

} // namespace

int main() {
  watchOnValidDirectoryReturnsId();
  watchOnMissingPathReturnsNullopt();
  watchOnEmptyPathReturnsNullopt();
  drainWithCallbackFiresOnEvent();
  drainWithCallbackFiresForAllWatches();
  unwatchSilencesCallbackAndKeepsObjectUsable();
  queueOverflowIsForwarded();
  return 0;
}
