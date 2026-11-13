#include "system/cpu_freq.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

namespace {

  std::filesystem::path gRoot;

  void removeRoot() { std::filesystem::remove_all(gRoot); }

  void writeStat(const std::filesystem::path& path, const std::string& value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out{path};
    out << value;
  }

} // namespace

int main() {
  char dirTemplate[] = "/tmp/noctalia_cpu_freq_test_XXXXXX";
  char* dir = mkdtemp(dirTemplate);
  if (dir == nullptr) {
    return EXIT_FAILURE;
  }
  gRoot = dir;
  std::atexit(removeRoot);
  const std::filesystem::path& root = gRoot;

  writeStat(root / "cpu0/cpufreq/scaling_cur_freq", "1800000");
  writeStat(root / "cpu1/cpufreq/scaling_cur_freq", "2400000");
  TEST_NEAR(noctalia::system::cpu_freq::readFreqs(root).curMhz, 2100.0, 0.5); // average across cores

  writeStat(root / "cpu0/cpufreq/scaling_cur_freq", "0");
  TEST_NEAR(noctalia::system::cpu_freq::readFreqs(root).curMhz, 2400.0, 0.5); // zero entries skipped

  const std::filesystem::path emptyRoot = root / "empty";
  std::filesystem::create_directory(emptyRoot);
  TEST_CHECK(!noctalia::system::cpu_freq::readFreqs(emptyRoot).curMhz.has_value());

  writeStat(root / "cpufreq/", ""); // stray non-cpuN dir at root must be ignored
  std::filesystem::create_directory(root / "offline");
  TEST_NEAR(noctalia::system::cpu_freq::readFreqs(root).curMhz, 2400.0, 0.5); // stray dirs ignored

  writeStat(root / "cpu0/cpufreq/scaling_max_freq", "4800000");
  TEST_NEAR(noctalia::system::cpu_freq::readFreqs(root).maxMhz, 4800.0, 0.5);

  TEST_CHECK(!noctalia::system::cpu_freq::readFreqs(emptyRoot).maxMhz.has_value());

  std::filesystem::remove_all(root / "cpu0/cpufreq");
  std::filesystem::remove_all(root / "cpu1/cpufreq");
  const auto unavailable = noctalia::system::cpu_freq::readFreqs(root);
  TEST_CHECK(!unavailable.curMhz.has_value());
  TEST_CHECK(!unavailable.maxMhz.has_value());
  return 0;
}