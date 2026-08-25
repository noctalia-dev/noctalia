#include "theme/hook_runner.h"

#include <cassert>
#include <chrono>

namespace {

  void test_hook_runner_basic() {
    noctalia::theme::HookRunner runner(2);

    runner.enqueue("true");
    runner.enqueue("true");

    runner.waitIdle();
    assert(runner.pendingCount() == 0);
  }

  void test_hook_runner_async() {
    noctalia::theme::HookRunner runner(2);

    auto start = std::chrono::steady_clock::now();

    runner.enqueue("sleep 1");
    runner.enqueue("true");

    auto enqueue_time = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(enqueue_time - start).count();

    // Enqueue should return immediately
    assert(elapsed < 50);

    runner.waitIdle();
  }

  void test_hook_runner_parallel() {
    noctalia::theme::HookRunner runner(4);

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < 4; ++i) {
      runner.enqueue("sleep 1");
    }

    runner.waitIdle();

    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // 4 hooks of 1s on 4 threads = ~1s (not 4s)
    assert(elapsed < 2000);
  }

} // namespace

int main() {
  test_hook_runner_basic();
  test_hook_runner_async();
  test_hook_runner_parallel();
  return 0;
}
