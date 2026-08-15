#include "core/inotify/inotify.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "inotify_test: FAIL: {}", message);
    }
    return condition;
  }

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

  bool watchOnValidDirectoryReturnsId() {
    Inotify in;
    const auto dir = uniqueTempDir();
    auto wd = in.watch(dir, IN_CREATE);
    const bool ok = expect(wd.has_value(), "watch() on a valid directory should return a WatchId");
    cleanup(dir);
    return ok;
  }

  bool watchOnMissingPathReturnsNullopt() {
    Inotify in;
    const auto dir = uniqueTempDir();
    const auto missing = dir / "does-not-exist";
    auto wd = in.watch(missing, IN_CREATE);
    const bool ok = expect(!wd.has_value(), "watch() on a missing path should return std::nullopt");
    cleanup(dir);
    return ok;
  }

  bool watchOnEmptyPathReturnsNullopt() {
    Inotify in;
    auto wd = in.watch(std::filesystem::path{}, IN_CREATE);
    return expect(!wd.has_value(), "watch() on an empty path should return std::nullopt");
  }

  bool drainWithCallbackFiresOnEvent() {
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

    const bool ok = expect(fired, "global drain callback should fire when an event occurs in a watched directory")
        && expect((mask & IN_CREATE) != 0, "global callback event mask should contain IN_CREATE")
        && expect(name == "child.txt", "global callback event name should carry the created file name");
    cleanup(dir);
    return ok;
  }

  bool drainWithCallbackFiresForAllWatches() {
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

    // No events yet — nothing should fire.
    if (!expect(globalHits == 0, "global callback should not fire before any events are generated")) {
      cleanup(dir);
      return false;
    }

    std::ofstream{dirA / "file_a.txt"} << "x";
    in.drain([&](const inotify_event*) { ++globalHits; });

    if (!expect(globalHits == 1, "global callback should fire once for an event in watch A")) {
      cleanup(dir);
      return false;
    }

    std::ofstream{dirB / "file_b.txt"} << "y";
    in.drain([&](const inotify_event*) { ++globalHits; });

    const bool ok = expect(globalHits == 2, "global callback should receive events from all watches");
    cleanup(dir);
    return ok;
  }

  bool unwatchSilencesCallbackAndKeepsObjectUsable() {
    Inotify in;
    const auto dir = uniqueTempDir();

    const auto wd = in.watch(dir, IN_CREATE);
    if (!expect(wd.has_value(), "watch should return a WatchId so it can be unwatched")) {
      cleanup(dir);
      return false;
    }

    // Drain any events queued while wiring up the watch so the count starts at zero.
    in.drain();

    int fired = 0;
    std::ofstream{dir / "a.txt"} << "1";
    in.drain([&](const inotify_event*) { ++fired; });
    if (!expect(fired == 1, "global callback should fire once for an event in a watched directory")) {
      cleanup(dir);
      return false;
    }

    // After unwatch, the same directory must stop delivering events to the callback.
    in.unwatch(*wd);
    std::ofstream{dir / "b.txt"} << "2";
    in.drain([&](const inotify_event*) { ++fired; });
    if (!expect(fired == 1, "no callback should fire after unwatch")) {
      cleanup(dir);
      return false;
    }

    // The object must remain usable: descriptor intact and a fresh watch succeeds.
    const auto wd2 = in.watch(dir, IN_CREATE);
    if (!expect(wd2.has_value(), "fresh watch should succeed after unwatch")) {
      cleanup(dir);
      return false;
    }
    if (!expect(in.fd() >= 0, "fd() should remain valid after unwatch")) {
      cleanup(dir);
      return false;
    }

    int firedAgain = 0;
    std::ofstream{dir / "c.txt"} << "3";
    in.drain([&](const inotify_event*) { ++firedAgain; });
    const bool ok = expect(firedAgain == 1, "re-watched directory should deliver events again");
    cleanup(dir);
    return ok;
  }

  bool queueOverflowIsForwarded() {
    Inotify in;
    const auto dir = uniqueTempDir();
    const auto fileA = dir / "a";
    const auto fileB = dir / "b";
    std::ofstream{fileA};
    std::ofstream{fileB};

    const auto wdA = in.watch(fileA, IN_OPEN);
    const auto wdB = in.watch(fileB, IN_OPEN);
    if (!expect(wdA.has_value() && wdB.has_value(), "overflow test files should be watchable")) {
      cleanup(dir);
      return false;
    }

    std::ifstream limitFile("/proc/sys/fs/inotify/max_queued_events");
    std::size_t maxQueuedEvents = 0;
    if (!expect(
            static_cast<bool>(limitFile >> maxQueuedEvents) && maxQueuedEvents > 0,
            "kernel inotify queue limit should be readable"
        )) {
      cleanup(dir);
      return false;
    }

    // Alternate watch descriptors so adjacent IN_OPEN events are not coalesced.
    for (std::size_t i = 0; i <= maxQueuedEvents; ++i) {
      std::ifstream opened{i % 2 == 0 ? fileA : fileB};
      if (!expect(opened.is_open(), "overflow test file should remain readable")) {
        cleanup(dir);
        return false;
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

    const bool ok = expect(overflowForwarded, "global callback should receive IN_Q_OVERFLOW")
        && expect(overflowWd == -1, "IN_Q_OVERFLOW should carry the global watch descriptor");
    cleanup(dir);
    return ok;
  }

} // namespace

int main() {
  bool ok = true;
  ok = watchOnValidDirectoryReturnsId() && ok;
  ok = watchOnMissingPathReturnsNullopt() && ok;
  ok = watchOnEmptyPathReturnsNullopt() && ok;
  ok = drainWithCallbackFiresOnEvent() && ok;
  ok = drainWithCallbackFiresForAllWatches() && ok;
  ok = unwatchSilencesCallbackAndKeepsObjectUsable() && ok;
  ok = queueOverflowIsForwarded() && ok;
  return ok ? 0 : 1;
}
