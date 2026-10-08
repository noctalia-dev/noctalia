#include "tests/test_check.h"
#include "theme/hook_runner.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <signal.h>
#include <string>
#include <sys/types.h>
#include <thread>

namespace {

  std::filesystem::path sentinelPath(const char* name) {
    return std::filesystem::temp_directory_path() / (std::string("noctalia_hook_runner_") + name);
  }

  std::string readSentinel(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string contents;
    std::getline(in, contents);
    return contents;
  }

  // Waits for a hook to report its pid, so a test knows the hook is really running.
  pid_t waitForPid(const std::filesystem::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      const std::string contents = readSentinel(path);
      if (!contents.empty()) {
        return static_cast<pid_t>(std::stol(contents));
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return -1;
  }

  void test_runs_every_enqueued_hook() {
    const auto sentinel = sentinelPath("run");
    std::filesystem::remove(sentinel);

    {
      noctalia::theme::HookRunner runner(2);
      for (int i = 0; i < 4; ++i) {
        runner.enqueue("printf x >> " + sentinel.string(), /*generation=*/1);
      }
      runner.waitIdle();
      TEST_CHECK(runner.pendingCount() == 0);
    }

    // Concurrency is bounded, so queued hooks must still all run before waitIdle returns.
    TEST_CHECK(readSentinel(sentinel) == "xxxx");
    std::filesystem::remove(sentinel);
  }

  void test_drops_hooks_from_superseded_generations() {
    const auto sentinel = sentinelPath("generation");
    std::filesystem::remove(sentinel);

    {
      noctalia::theme::HookRunner runner(2);
      runner.invalidateBefore(2);
      // Generation 1 is already superseded: the hook must never run.
      runner.enqueue("printf stale > " + sentinel.string(), /*generation=*/1);
      runner.enqueue("printf current > " + sentinel.string(), /*generation=*/2);
      runner.waitIdle();
      TEST_CHECK(runner.pendingCount() == 0);
    }

    TEST_CHECK(readSentinel(sentinel) == "current");
    std::filesystem::remove(sentinel);
  }

  void test_invalidate_drops_queued_hooks() {
    const auto sentinel = sentinelPath("invalidate");
    std::filesystem::remove(sentinel);

    {
      // A single slot occupied by a long hook keeps the rest of the batch queued, so
      // invalidateBefore() has to discard them.
      noctalia::theme::HookRunner runner(1);
      runner.enqueue("sleep 0.2", /*generation=*/1);
      runner.enqueue("printf stale > " + sentinel.string(), /*generation=*/1);
      runner.invalidateBefore(2);
      runner.waitIdle();
      TEST_CHECK(runner.pendingCount() == 0);
    }

    TEST_CHECK(!std::filesystem::exists(sentinel));
  }

  void test_shutdown_drops_backlog_and_awaits_running() {
    const auto running = sentinelPath("shutdown_running");
    const auto queued = sentinelPath("shutdown_queued");
    std::filesystem::remove(running);
    std::filesystem::remove(queued);

    {
      noctalia::theme::HookRunner runner(1);
      runner.enqueue("sleep 0.2; printf ran > " + running.string(), /*generation=*/1);
      runner.enqueue("printf ran > " + queued.string(), /*generation=*/1);
      runner.requestShutdown();
      // waitIdle() must not block on the discarded backlog after a shutdown request.
      runner.waitIdle();
    }

    // Destruction waits for the hook that had already started, and only for that one.
    TEST_CHECK(std::filesystem::exists(running));
    TEST_CHECK(!std::filesystem::exists(queued));
    std::filesystem::remove(running);
  }

  // An owner bounds shutdown by raising the cancel flag it passed in: that terminates the
  // running hook, so the destructor no longer waits for it to exit on its own.
  void test_cancel_terminates_running_hook() {
    const auto pidFile = sentinelPath("shared_pid");
    std::filesystem::remove(pidFile);

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto runner = std::make_unique<noctalia::theme::HookRunner>(1, cancel);
    // Ignores SIGTERM, so only the SIGKILL escalation can end it.
    runner->enqueue("trap '' TERM; echo $$ > " + pidFile.string() + "; exec sleep 600", /*generation=*/1);
    const pid_t pid = waitForPid(pidFile);
    TEST_CHECK(pid > 0);

    const auto start = std::chrono::steady_clock::now();
    std::thread owner([cancel]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      cancel->store(true);
    });
    runner.reset();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    owner.join();

    TEST_CHECK(elapsed < std::chrono::seconds(2));
    TEST_CHECK(::kill(pid, 0) != 0 && errno == ESRCH);
    std::filesystem::remove(pidFile);
  }

} // namespace

int main() {
  test_runs_every_enqueued_hook();
  test_drops_hooks_from_superseded_generations();
  test_invalidate_drops_queued_hooks();
  test_shutdown_drops_backlog_and_awaits_running();
  test_cancel_terminates_running_hook();
  return 0;
}
