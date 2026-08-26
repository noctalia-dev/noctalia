#include "theme/hook_runner.h"

#include <cassert>
#include <chrono>
#include <cstdint>

namespace {
  constexpr std::uint64_t kGeneration = 1;

  void test_hook_runner_basic() {
    noctalia::theme::HookRunner runner(2);

    runner.enqueue("true", kGeneration);
    runner.enqueue("true", kGeneration);

    runner.waitIdle();
    assert(runner.pendingCount() == 0);
  }

  void test_hook_runner_async() {
    noctalia::theme::HookRunner runner(2);

    auto start = std::chrono::steady_clock::now();

    runner.enqueue("sleep 1", kGeneration);
    runner.enqueue("true", kGeneration);

    auto enqueue_time = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(enqueue_time - start).count();

    // Enqueue should return immediately
    assert(elapsed < 50);
    (void)elapsed;

    runner.waitIdle();
  }

  void test_hook_runner_parallel() {
    noctalia::theme::HookRunner runner(4);

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < 4; ++i) {
      runner.enqueue("sleep 1", kGeneration);
    }

    runner.waitIdle();

    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // 4 hooks of 1s on 4 threads = ~1s (not 4s)
    assert(elapsed < 2000);
    (void)elapsed;
  }

  void test_hook_runner_invalidate() {
    noctalia::theme::HookRunner runner(2);

    runner.enqueue("true", /*generation=*/1);
    runner.invalidateBefore(2);
    runner.enqueue("true", /*generation=*/2);

    runner.waitIdle();
    assert(runner.pendingCount() == 0);
  }

} // namespace

int main() {
  test_hook_runner_basic();
  test_hook_runner_async();
  test_hook_runner_parallel();
  test_hook_runner_invalidate();
  return 0;
}
