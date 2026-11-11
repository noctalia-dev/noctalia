// Covers the /proc/stat parsing and delta maths behind SystemStats::cpuUsagePercent and
// SystemStats::cpuCoreUsagePercent. Both readers take the stat path so fixtures stand in for the
// live /proc.

#include "system/cpu_stat.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

  namespace cpu_stat = noctalia::system::cpu_stat;
  using cpu_stat::Totals;

  std::filesystem::path makeTempDir() {
    std::string pattern = (std::filesystem::temp_directory_path() / "noctalia-cpu-stat-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const char* result = ::mkdtemp(writable.data());
    return result != nullptr ? std::filesystem::path(result) : std::filesystem::path{};
  }

  std::filesystem::path writeStat(const std::filesystem::path& dir, std::string_view name, std::string_view text) {
    const auto path = dir / name;
    std::ofstream out(path, std::ios::trunc);
    out << text;
    return path;
  }

  void testParseLine() {
    // user nice system idle iowait irq softirq steal -> idle = idle + iowait,
    // total = every field summed.
    const auto totals = cpu_stat::parseLine("cpu  100 20 30 700 50 5 5 10", "cpu");
    TEST_CHECK(totals.has_value());
    // idle should fold iowait in (700 + 50)
    TEST_CHECK(totals->idle == 750);
    TEST_CHECK(totals->total == 920);

    // The label is pinned, so the aggregate row can never be mistaken for core 0.
    TEST_CHECK(!cpu_stat::parseLine("cpu  100 20 30 700 50 5 5 10", "cpu0").has_value());
    TEST_CHECK(!cpu_stat::parseLine("cpu0 100 20 30 700 50 5 5 10", "cpu").has_value());
    TEST_CHECK(cpu_stat::parseLine("cpu7 1 2 3 4 5 6 7 8", "cpu7").has_value());

    // Non-cpu rows in /proc/stat must not parse as totals.
    TEST_CHECK(!cpu_stat::parseLine("intr 12345 0 0", "cpu").has_value());

    // Kernels predating the steal field still yield usable totals: the missing trailing fields
    // stay zero-initialised rather than poisoning the sum.
    const auto shortRow = cpu_stat::parseLine("cpu  100 20 30 700 50 5 5", "cpu");
    TEST_CHECK(shortRow.has_value());
    TEST_CHECK(shortRow->total == 910);
  }

  void testUsageBetween() {
    // 100 jiffies elapsed, 25 of them idle -> 75% busy.
    TEST_NEAR(
        cpu_stat::usageBetween(Totals{.total = 1000, .idle = 800}, Totals{.total = 1100, .idle = 825}), 75.0, 0.001
    );
    TEST_NEAR(
        cpu_stat::usageBetween(Totals{.total = 1000, .idle = 800}, Totals{.total = 1100, .idle = 900}), 0.0, 0.001
    );
    TEST_NEAR(
        cpu_stat::usageBetween(Totals{.total = 1000, .idle = 800}, Totals{.total = 1100, .idle = 800}), 100.0, 0.001
    );

    // An empty window has no answer; reporting 0% would render as "idle", which is a lie.
    TEST_CHECK(
        !cpu_stat::usageBetween(Totals{.total = 1000, .idle = 800}, Totals{.total = 1000, .idle = 800}).has_value()
    );

    // Counters going backwards (suspend/resume, container reset) must not underflow into a vast
    // bogus percentage.
    TEST_CHECK(
        !cpu_stat::usageBetween(Totals{.total = 2000, .idle = 900}, Totals{.total = 1000, .idle = 400}).has_value()
    );
    TEST_NEAR(
        cpu_stat::usageBetween(Totals{.total = 1000, .idle = 900}, Totals{.total = 1100, .idle = 500}), 100.0, 0.001
    );
  }

  void testReadFixtures() {
    const auto dir = makeTempDir();
    TEST_CHECK(!dir.empty());

    constexpr std::string_view kStat = "cpu  400 0 100 500 0 0 0 0\n"
                                       "cpu0 100 0 25 125 0 0 0 0\n"
                                       "cpu1 100 0 25 125 0 0 0 0\n"
                                       "cpu2 100 0 25 125 0 0 0 0\n"
                                       "cpu3 100 0 25 125 0 0 0 0\n"
                                       "intr 999 0 0\n"
                                       "ctxt 4242\n";
    const auto path = writeStat(dir, "stat", kStat);

    const auto aggregate = cpu_stat::readTotals(path);
    const auto cores = cpu_stat::readCoreTotals(path);

    // Offlining a core removes its row, leaving a gap in the cpuN numbering. Every remaining core
    // must still be reported: deriving the expected label from the count so far would desync at
    // the gap and silently drop every core after it.
    constexpr std::string_view kOfflined = "cpu  400 0 100 500 0 0 0 0\n"
                                           "cpu0 100 0 25 125 0 0 0 0\n"
                                           "cpu1 100 0 25 125 0 0 0 0\n"
                                           "cpu3 100 0 25 125 0 0 0 0\n"
                                           "cpu5 100 0 25 125 0 0 0 0\n"
                                           "intr 999 0 0\n";
    const auto gapped = cpu_stat::readCoreTotals(writeStat(dir, "stat-offlined", kOfflined));

    // A machine with a single core still reports a cpu0 row.
    const auto single =
        cpu_stat::readCoreTotals(writeStat(dir, "stat-single", "cpu  8 0 0 2 0 0 0 0\ncpu0 8 0 0 2 0 0 0 0\n"));

    // Aggregate row only (no per-core rows) is not an empty list but a failure: callers must be
    // able to tell "no per-core data" from "zero cores".
    const auto noCores = cpu_stat::readCoreTotals(writeStat(dir, "stat-nocores", "cpu  8 0 0 2 0 0 0 0\nintr 1\n"));

    // A cpuN row that does not parse must not shorten the vector silently.
    const auto broken =
        cpu_stat::readCoreTotals(writeStat(dir, "stat-broken", "cpu  8 0 0 2 0 0 0 0\ncpu0 8 0 0 2\ncpu1 x\n"));

    const auto missing = cpu_stat::readTotals(dir / "does-not-exist");
    const auto missingCores = cpu_stat::readCoreTotals(dir / "does-not-exist");
    const auto empty = cpu_stat::readTotals(writeStat(dir, "stat-empty", ""));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    TEST_CHECK(aggregate.has_value());
    TEST_CHECK(aggregate->total == 1000);
    TEST_CHECK(aggregate->idle == 500);

    TEST_CHECK(cores.has_value());
    // Four cores, not five: the leading aggregate row is skipped and the trailing non-cpu rows
    // are not mistaken for cores.
    TEST_CHECK(cores->size() == 4);
    TEST_CHECK((*cores)[0].total == 250 && (*cores)[0].idle == 125);
    TEST_CHECK((*cores)[3].total == 250 && (*cores)[3].idle == 125);

    TEST_CHECK(gapped.has_value() && gapped->size() == 4);
    TEST_CHECK(single.has_value() && single->size() == 1);
    TEST_CHECK(!noCores.has_value());
    TEST_CHECK(!broken.has_value());
    TEST_CHECK(!missing.has_value());
    TEST_CHECK(!missingCores.has_value());
    TEST_CHECK(!empty.has_value());
  }

  // The per-core sampling loop diffs two whole vectors; this is that maths in isolation.
  void testPerCoreDeltas() {
    const std::vector<Totals> prev = {{.total = 100, .idle = 100}, {.total = 100, .idle = 100}};
    const std::vector<Totals> next = {{.total = 200, .idle = 100}, {.total = 200, .idle = 200}};
    TEST_NEAR(cpu_stat::usageBetween(prev[0], next[0]), 100.0, 0.001);
    TEST_NEAR(cpu_stat::usageBetween(prev[1], next[1]), 0.0, 0.001);
  }

} // namespace

int main() {
  testParseLine();
  testUsageBetween();
  testReadFixtures();
  testPerCoreDeltas();
  return 0;
}
