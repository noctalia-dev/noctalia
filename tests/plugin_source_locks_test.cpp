#include "scripting/plugin_source_locks.h"

#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <print>
#include <thread>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "plugin_source_locks_test: {}", message);
    }
    return condition;
  }

} // namespace

int main() {
  bool ok = true;

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
    worker = std::thread([attempted, acquired]() {
      attempted->set_value();
      auto workerGuard = scripting::plugin_source_locks::acquire("community");
      (void)workerGuard;
      acquired->set_value();
    });

    attemptedFuture.wait();
    ok = expect(
             acquiredFuture.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready,
             "same-source lock acquired while already held"
         )
        && ok;
    (void)guard;
  }

  const bool released = acquiredFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  ok = expect(released, "same-source lock was not released") && ok;
  if (released) {
    worker.join();
  } else {
    worker.detach();
  }

  return ok ? 0 : 1;
}
