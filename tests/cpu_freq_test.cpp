#include "system/cpu_freq.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <print>
#include <string>
#include <system_error>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition)
      std::println(stderr, "FAIL: {}", message);
    return condition;
  }

  bool expectNear(const std::optional<double>& value, double expected, const char* message) {
    if (!value.has_value()) {
      std::println(stderr, "FAIL: {} (no value)", message);
      return false;
    }
    if (std::abs(*value - expected) > 0.5) {
      std::println(stderr, "FAIL: {} (got {}, want {})", message, *value, expected);
      return false;
    }
    return true;
  }

  void writeStat(const std::filesystem::path& path, const std::string& value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out{path};
    out << value;
  }

} // namespace

int main() {
  char dirTemplate[] = "/tmp/noctalia_cpu_freq_test_XXXXXX";
  char* dir = mkdtemp(dirTemplate);
  if (dir == nullptr)
    return EXIT_FAILURE;
  const std::filesystem::path root{dir};
  bool ok = true;

  writeStat(root / "cpu0/cpufreq/scaling_cur_freq", "1800000");
  writeStat(root / "cpu1/cpufreq/scaling_cur_freq", "2400000");
  ok = expectNear(noctalia::system::cpu_freq::readFreqs(root).curMhz.value_or(0.0), 2100.0, "average across cores")
      && ok;

  writeStat(root / "cpu0/cpufreq/scaling_cur_freq", "0");
  ok = expectNear(noctalia::system::cpu_freq::readFreqs(root).curMhz.value_or(0.0), 2400.0, "zero entries skipped")
      && ok;

  const std::filesystem::path emptyRoot = root / "empty";
  std::filesystem::create_directory(emptyRoot);
  ok = expect(!noctalia::system::cpu_freq::readFreqs(emptyRoot).curMhz.has_value(), "empty root yields nullopt") && ok;

  writeStat(root / "cpufreq/", ""); // stray non-cpuN dir at root must be ignored
  std::filesystem::create_directory(root / "offline");
  ok = expectNear(noctalia::system::cpu_freq::readFreqs(root).curMhz.value_or(0.0), 2400.0, "stray dirs ignored") && ok;

  writeStat(root / "cpu0/cpufreq/scaling_max_freq", "4800000");
  ok = expectNear(noctalia::system::cpu_freq::readFreqs(root).maxMhz, 4800.0, "max freq from scaling_max_freq") && ok;

  ok = expect(!noctalia::system::cpu_freq::readFreqs(emptyRoot).maxMhz.has_value(), "no max freq yields nullopt") && ok;

  std::filesystem::remove_all(root / "cpu0/cpufreq");
  std::filesystem::remove_all(root / "cpu1/cpufreq");
  const auto unavailable = noctalia::system::cpu_freq::readFreqs(root);
  ok = expect(!unavailable.curMhz.has_value(), "removed cpufreq data clears current frequency") && ok;
  ok = expect(!unavailable.maxMhz.has_value(), "removed cpufreq data clears maximum frequency") && ok;

  std::filesystem::remove_all(root);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}