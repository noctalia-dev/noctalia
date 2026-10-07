// Covers the demand-driven wake scheduling in TimeService: the poll timeout is
// derived from the published Precision, and the tick callback fires only when a
// second boundary has actually elapsed.

#include "test_check.h"
#include "time/time_service.h"

#include <chrono>
#include <thread>

namespace {

  using namespace std::chrono;

  // Sleep until comfortably past the next second boundary so tick() sees a new
  // whole second regardless of where in the current second the test started.
  void sleepPastNextSecond() {
    const auto next = floor<seconds>(system_clock::now()) + seconds{1};
    std::this_thread::sleep_until(next + milliseconds{5});
  }

} // namespace

int main() {
  {
    TimeService service;
    const int timeout = service.pollTimeoutMs();
    TEST_CHECK(timeout >= 1);
    TEST_CHECK(timeout <= 60000);
  }

  {
    TimeService service;
    service.setPrecisionProvider([] { return TimeService::Precision::Second; });
    const int timeout = service.pollTimeoutMs();
    TEST_CHECK(timeout >= 1);
    TEST_CHECK(timeout <= 1000);
  }

  {
    TimeService service;
    int ticks = 0;
    service.setTickCallback([&ticks] { ++ticks; });

    service.tick();
    const int afterFirstTick = ticks;
    // The second tick() runs in the same wall-clock second, so the boundary has
    // not moved and the callback must not fire again.
    service.tick();
    TEST_CHECK(ticks == afterFirstTick);

    sleepPastNextSecond();
    service.tick();
    TEST_CHECK(ticks == afterFirstTick + 1);

    // now() tracks the latest tick.
    TEST_CHECK(service.now() >= system_clock::now() - seconds{1});
  }

  return 0;
}
