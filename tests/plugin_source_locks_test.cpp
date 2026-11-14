#include "scripting/plugin_source_locks.h"
#include "tests/test_check.h"

#include <chrono>
#include <future>
#include <memory>
#include <thread>

int main() {
  {
    auto outer = scripting::plugin_source_locks::acquire("official");
    auto inner = scripting::plugin_source_locks::acquire("official");
    (void)outer;
    (void)inner;
  }

  auto attempted = std::make_shared<std::promise<void>>();
  auto acquired = std::make_shared<std::promise<void>>();
  auto attemptedFuture = attempted->get_future();
  auto acquiredFuture = acquired->get_future();

  std::thread worker;
  {
    auto guard = scripting::plugin_source_locks::acquire("community");
    worker = std::thread([attempted, acquired] {
      attempted->set_value();
      auto workerGuard = scripting::plugin_source_locks::acquire("community");
      (void)workerGuard;
      acquired->set_value();
    });

    attemptedFuture.wait();
    TEST_CHECK(acquiredFuture.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready);
    (void)guard;
  }

  const bool released = acquiredFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  TEST_CHECK(released);
  worker.join();

  return 0;
}
