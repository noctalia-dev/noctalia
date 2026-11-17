#include "filemanager/transfer_rates.h"
#include "tests/test_check.h"

#include <chrono>
#include <limits>

int main() {
  using namespace std::chrono_literals;
  using filemanager::TransferRates;
  const auto origin = TransferRates::Clock::time_point{};
  TransferRates rates;
  rates.observe(origin, 0, false);
  TEST_CHECK(rates.displaySpeed() == 0 && rates.etaSpeed() == 0);
  rates.observe(origin + 100ms, 100, false);
  TEST_CHECK(rates.displaySpeed() == 1000);
  rates.observe(origin + 1s, 100, false);
  TEST_CHECK(rates.displaySpeed() == 100); // Not (1000 + 0) / 2.

  TransferRates interpolated;
  interpolated.observe(origin, 0, false);
  interpolated.observe(origin + 800ms, 800, false);
  interpolated.observe(origin + 1500ms, 940, false);
  TEST_CHECK(interpolated.displaySpeed() == 440); // Interpolate the one-second boundary.

  TransferRates changing;
  std::uint64_t bytes = 0;
  changing.observe(origin, bytes, false);
  for (int i = 1; i <= 50; ++i) {
    bytes += 100;
    changing.observe(origin + i * 100ms, bytes, false);
  }
  TEST_CHECK(changing.displaySpeed() == 1000 && changing.etaSpeed() == 1000);
  for (int i = 51; i <= 60; ++i) {
    bytes += 10;
    changing.observe(origin + i * 100ms, bytes, false);
  }
  TEST_CHECK(changing.displaySpeed() == 100);
  TEST_CHECK(changing.etaSpeed() == 820);
  for (int i = 61; i <= 100; ++i) {
    bytes += 10;
    changing.observe(origin + i * 100ms, bytes, false);
  }
  TEST_CHECK(changing.etaSpeed() == 100);
  changing.observe(origin + 11s, bytes, false);
  TEST_CHECK(changing.displaySpeed() == 0 && changing.etaSpeed() == 80);
  changing.observe(origin + 15s, bytes, false);
  TEST_CHECK(changing.etaSpeed() == 0); // Stalled ETA becomes unknown, never divides by zero.
  changing.observe(origin + 16s, bytes, true);
  TEST_CHECK(changing.displaySpeed() == 0 && changing.etaSpeed() == 0);
  changing.observe(origin + 60s, bytes, true);
  changing.observe(origin + 61s, bytes, false);
  TEST_CHECK(changing.etaSpeed() == 0);
  changing.observe(origin + 61200ms, bytes + 200, false);
  TEST_CHECK(changing.displaySpeed() == 1000 && changing.etaSpeed() == 1000);
  changing.observe(origin + 62s, 0, false); // Counter reset starts a new baseline.
  TEST_CHECK(changing.displaySpeed() == 0);
  changing.observe(origin + 63s, 500, false);
  TEST_CHECK(changing.displaySpeed() == 500);
  changing.observe(origin + 62s, std::numeric_limits<std::uint64_t>::max(), false);
  TEST_CHECK(changing.displaySpeed() == 500); // Out-of-order clocks do not corrupt the counter window.
}
