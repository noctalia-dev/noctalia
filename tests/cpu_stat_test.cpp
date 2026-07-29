// Covers the /proc/stat parsing and delta maths behind SystemStats::cpuUsagePercent and
// SystemStats::cpuCoreUsagePercent. Both readers take the stat path so fixtures stand in for the
// live /proc.

#include "system/cpu_stat.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

  namespace cpu_stat = noctalia::system::cpu_stat;
  using Totals = cpu_stat::Totals;

  int g_failures = 0;

  bool expect(bool condition, const std::string& message) {
    if (!condition) {
      std::fprintf(stderr, "cpu_stat_test: FAIL: %s\n", message.c_str());
      ++g_failures;
      return false;
    }
    return true;
  }

  bool expectNear(std::optional<double> actual, double expected, const std::string& message) {
    if (!actual.has_value()) {
      std::fprintf(stderr, "cpu_stat_test: FAIL: %s: no value\n", message.c_str());
      ++g_failures;
      return false;
    }
    const double diff = *actual > expected ? *actual - expected : expected - *actual;
    if (diff > 0.001) {
      std::fprintf(stderr, "cpu_stat_test: FAIL: %s: expected %f, got %f\n", message.c_str(), expected, *actual);
      ++g_failures;
      return false;
    }
    return true;
  }

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
    expect(totals.has_value(), "aggregate row should parse");
    if (totals.has_value()) {
      expect(totals->idle == 750, "idle should fold iowait in (700 + 50)");
      expect(totals->total == 920, "total should sum all eight fields");
    }

    // The label is pinned, so the aggregate row can never be mistaken for core 0.
    expect(!cpu_stat::parseLine("cpu  100 20 30 700 50 5 5 10", "cpu0").has_value(), "'cpu' must not match 'cpu0'");
    expect(!cpu_stat::parseLine("cpu0 100 20 30 700 50 5 5 10", "cpu").has_value(), "'cpu0' must not match 'cpu'");
    expect(cpu_stat::parseLine("cpu7 1 2 3 4 5 6 7 8", "cpu7").has_value(), "matching core label should parse");

    // Non-cpu rows in /proc/stat must not parse as totals.
    expect(!cpu_stat::parseLine("intr 12345 0 0", "cpu").has_value(), "intr row should not parse");

    // Kernels predating the steal field still yield usable totals: the missing trailing fields
    // stay zero-initialised rather than poisoning the sum.
    const auto shortRow = cpu_stat::parseLine("cpu  100 20 30 700 50 5 5", "cpu");
    expect(shortRow.has_value(), "row without steal should still parse");
    if (shortRow.has_value()) {
      expect(shortRow->total == 910, "total should omit the absent steal field");
    }
  }

  void testUsageBetween() {
    // 100 jiffies elapsed, 25 of them idle -> 75% busy.
    expectNear(
        cpu_stat::usageBetween(Totals{.total = 1000, .idle = 800}, Totals{.total = 1100, .idle = 825}), 75.0, "75% busy"
    );
    expectNear(
        cpu_stat::usageBetween(Totals{.total = 1000, .idle = 800}, Totals{.total = 1100, .idle = 900}), 0.0,
        "fully idle"
    );
    expectNear(
        cpu_stat::usageBetween(Totals{.total = 1000, .idle = 800}, Totals{.total = 1100, .idle = 800}), 100.0,
        "fully busy"
    );

    // An empty window has no answer; reporting 0% would render as "idle", which is a lie.
    expect(
        !cpu_stat::usageBetween(Totals{.total = 1000, .idle = 800}, Totals{.total = 1000, .idle = 800}).has_value(),
        "no elapsed jiffies should yield nullopt"
    );

    // Counters going backwards (suspend/resume, container reset) must not underflow into a vast
    // bogus percentage.
    expect(
        !cpu_stat::usageBetween(Totals{.total = 2000, .idle = 900}, Totals{.total = 1000, .idle = 400}).has_value(),
        "total going backwards should yield nullopt"
    );
    expectNear(
        cpu_stat::usageBetween(Totals{.total = 1000, .idle = 900}, Totals{.total = 1100, .idle = 500}), 100.0,
        "idle going backwards should clamp to 100, not wrap"
    );
  }

  void testReadFixtures(const std::filesystem::path& dir) {
    constexpr std::string_view kStat = "cpu  400 0 100 500 0 0 0 0\n"
                                       "cpu0 100 0 25 125 0 0 0 0\n"
                                       "cpu1 100 0 25 125 0 0 0 0\n"
                                       "cpu2 100 0 25 125 0 0 0 0\n"
                                       "cpu3 100 0 25 125 0 0 0 0\n"
                                       "intr 999 0 0\n"
                                       "ctxt 4242\n";
    const auto path = writeStat(dir, "stat", kStat);

    const auto aggregate = cpu_stat::readTotals(path);
    expect(aggregate.has_value(), "aggregate should read from a fixture");
    if (aggregate.has_value()) {
      expect(aggregate->total == 1000, "aggregate total");
      expect(aggregate->idle == 500, "aggregate idle");
    }

    const auto cores = cpu_stat::readCoreTotals(path);
    expect(cores.has_value(), "cores should read from a fixture");
    if (cores.has_value()) {
      // Four cores, not five: the leading aggregate row is skipped and the trailing non-cpu rows
      // are not mistaken for cores.
      expect(cores->size() == 4, "should find exactly 4 cores, skipping the aggregate row");
      if (cores->size() == 4) {
        expect((*cores)[0].total == 250 && (*cores)[0].idle == 125, "core 0 totals");
        expect((*cores)[3].total == 250 && (*cores)[3].idle == 125, "core 3 totals");
      }
    }

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
    expect(gapped.has_value() && gapped->size() == 4, "non-contiguous cpuN labels should all be reported");

    // A machine with a single core still reports a cpu0 row.
    const auto single =
        cpu_stat::readCoreTotals(writeStat(dir, "stat-single", "cpu  8 0 0 2 0 0 0 0\ncpu0 8 0 0 2 0 0 0 0\n"));
    expect(single.has_value() && single->size() == 1, "single-core machine should yield one entry");

    // Aggregate row only (no per-core rows) is not an empty list but a failure: callers must be
    // able to tell "no per-core data" from "zero cores".
    expect(
        !cpu_stat::readCoreTotals(writeStat(dir, "stat-nocores", "cpu  8 0 0 2 0 0 0 0\nintr 1\n")).has_value(),
        "stat without cpuN rows should yield nullopt"
    );

    // A cpuN row that does not parse must not shorten the vector silently.
    expect(
        !cpu_stat::readCoreTotals(writeStat(dir, "stat-broken", "cpu  8 0 0 2 0 0 0 0\ncpu0 8 0 0 2\ncpu1 x\n"))
             .has_value(),
        "an unparseable cpuN row should yield nullopt"
    );

    expect(!cpu_stat::readTotals(dir / "does-not-exist").has_value(), "missing file should yield nullopt");
    expect(
        !cpu_stat::readCoreTotals(dir / "does-not-exist").has_value(), "missing file should yield nullopt for cores"
    );
    expect(!cpu_stat::readTotals(writeStat(dir, "stat-empty", "")).has_value(), "empty file should yield nullopt");
  }

  // The per-core sampling loop diffs two whole vectors; this is that maths in isolation.
  void testPerCoreDeltas() {
    const std::vector<Totals> prev = {{.total = 100, .idle = 100}, {.total = 100, .idle = 100}};
    const std::vector<Totals> next = {{.total = 200, .idle = 100}, {.total = 200, .idle = 200}};
    expectNear(cpu_stat::usageBetween(prev[0], next[0]), 100.0, "core 0 fully busy");
    expectNear(cpu_stat::usageBetween(prev[1], next[1]), 0.0, "core 1 fully idle");
  }

} // namespace

int main() {
  const auto dir = makeTempDir();
  if (dir.empty()) {
    std::fprintf(stderr, "cpu_stat_test: FAIL: could not create temp dir\n");
    return 1;
  }

  testParseLine();
  testUsageBetween();
  testReadFixtures(dir);
  testPerCoreDeltas();

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);

  if (g_failures > 0) {
    std::fprintf(stderr, "cpu_stat_test: %d failure(s)\n", g_failures);
    return 1;
  }
  return 0;
}
