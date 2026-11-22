#include "config/atomic_file.h"
#include "core/files/file_watcher.h"
#include "core/timer_manager.h"
#include "shell/greeter/greeter_profile_auto_sync.h"
#include "shell/greeter/greeter_profile_store.h"
#include "tests/test_check.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <poll.h>
#include <string>
#include <unistd.h>

namespace {
  using Clock = std::chrono::steady_clock;
  using Launch = greeter::GreeterSyncLaunch;

  void pump(FileWatcher& watcher, std::chrono::milliseconds duration) {
    const auto deadline = Clock::now() + duration;
    while (Clock::now() < deadline) {
      pollfd fd{.fd = watcher.fd(), .events = POLLIN};
      if (::poll(&fd, 1, 10) > 0) {
        watcher.dispatch();
      }
      TimerManager::instance().tick();
    }
  }
} // namespace

int main() {
  static const auto root =
      std::filesystem::temp_directory_path() / ("noctalia-greeter-auto-sync-" + std::to_string(::getpid()));
  std::filesystem::create_directories(root);
  std::atexit([] { std::filesystem::remove_all(root); });
  const auto path = root / "profile/greeter.toml";
  FileWatcher watcher;
  int launches = 0;
  auto result = Launch::LaunchedConstrained;
  double appliedWidth = 0;
  GreeterProfileAutoSync sync(watcher, path, [&] {
    ++launches;
    const auto saved = greeter::readLocalProfile(path);
    TEST_CHECK(saved && saved->layout);
    appliedWidth = *saved->layout->components.at("login").width;
    return result;
  });
  GreeterProfile profile;
  profile.layout.emplace();
  profile.layout->components["login"].width = 400;
  const auto save = [&](double width) {
    profile.layout->components["login"].width = width;
    TEST_CHECK(greeter::writeLocalProfile(path, profile));
    pump(watcher, std::chrono::milliseconds(40));
  };

  sync.setEnabled(true);
  pump(watcher, std::chrono::milliseconds(1100));
  TEST_CHECK(launches == 0); // Enabling never applies an existing/default profile.
  save(420);
  pump(watcher, std::chrono::milliseconds(450));
  save(460);
  pump(watcher, std::chrono::milliseconds(450));
  TEST_CHECK(launches == 0);
  pump(watcher, std::chrono::milliseconds(650));
  TEST_CHECK(launches == 1 && appliedWidth == 460);
  save(460);
  sync.setEnabled(true); // Unrelated config reloads retain the observed baseline.
  pump(watcher, std::chrono::milliseconds(1100));
  TEST_CHECK(launches == 1);

  save(480);
  sync.setEnabled(false);
  pump(watcher, std::chrono::milliseconds(1100));
  TEST_CHECK(launches == 1);
  save(500);
  sync.setEnabled(true);
  pump(watcher, std::chrono::milliseconds(1100));
  TEST_CHECK(launches == 1); // Changes while disabled are not queued for later.

  profile.layout->components["login"].width = 520;
  TEST_CHECK(greeter::writeLocalProfile(path, profile));
  sync.acknowledgeSavedProfile(); // Explicit Save & apply consumes the delayed watch event too.
  save(520);
  pump(watcher, std::chrono::milliseconds(1100));
  TEST_CHECK(launches == 1);

  result = Launch::Busy;
  save(540);
  pump(watcher, std::chrono::milliseconds(1100));
  TEST_CHECK(launches == 2);
  save(560);
  result = Launch::LaunchedConstrained;
  pump(watcher, std::chrono::milliseconds(1100));
  TEST_CHECK(launches == 3 && appliedWidth == 560);
  pump(watcher, std::chrono::milliseconds(1100));
  TEST_CHECK(launches == 3); // Launch completion/cancellation cannot retry itself.

  result = Launch::Failed;
  save(580);
  pump(watcher, std::chrono::milliseconds(1100));
  TEST_CHECK(launches == 4);
  pump(watcher, std::chrono::milliseconds(1100));
  TEST_CHECK(launches == 4);

  save(600);
  TEST_CHECK(writeTextFileAtomic(path, "[widgets]\nwidget_order='invalid'\n"));
  pump(watcher, std::chrono::milliseconds(1100));
  TEST_CHECK(launches == 4); // Malformed external edits cancel pending valid work.
  std::filesystem::remove(path);
  save(600);
  result = Launch::LaunchedConstrained;
  pump(watcher, std::chrono::milliseconds(1100));
  TEST_CHECK(launches == 5 && appliedWidth == 600);

  save(620);
  sync.setEnabled(false);
  pump(watcher, std::chrono::milliseconds(1100));
  TEST_CHECK(launches == 5);
}
