#include "theme/hook_runner.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

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

  void test_runs_every_enqueued_hook() {
    const auto sentinel = sentinelPath("run");
    std::filesystem::remove(sentinel);

    {
      noctalia::theme::HookRunner runner(2);
      for (int i = 0; i < 4; ++i) {
        runner.enqueue("printf x >> " + sentinel.string(), /*generation=*/1);
      }
      runner.waitIdle();
      assert(runner.pendingCount() == 0);
    }

    // Concurrency is bounded, so queued hooks must still all run before waitIdle returns.
    assert(readSentinel(sentinel) == "xxxx");
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
      assert(runner.pendingCount() == 0);
    }

    assert(readSentinel(sentinel) == "current");
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
      assert(runner.pendingCount() == 0);
    }

    assert(!std::filesystem::exists(sentinel));
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
    assert(std::filesystem::exists(running));
    assert(!std::filesystem::exists(queued));
    std::filesystem::remove(running);
  }

} // namespace

int main() {
  test_runs_every_enqueued_hook();
  test_drops_hooks_from_superseded_generations();
  test_invalidate_drops_queued_hooks();
  test_shutdown_drops_backlog_and_awaits_running();
  return 0;
}
