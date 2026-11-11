#include "system/cpu_temp_sensor.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

  std::filesystem::path makeTempDir() {
    std::string pattern = (std::filesystem::temp_directory_path() / "noctalia-cpu-temp-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* result = ::mkdtemp(writable.data());
    return result != nullptr ? std::filesystem::path(result) : std::filesystem::path{};
  }

  void writeText(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << text;
  }

  struct SensorSpec {
    int index = 1;
    std::string label;
    int raw = 0;
  };

  std::filesystem::path addHwmon(
      const std::filesystem::path& hwmonRoot, std::string_view dirName, std::string_view name,
      const std::vector<SensorSpec>& sensors
  ) {
    const std::filesystem::path hwmon = hwmonRoot / dirName;
    writeText(hwmon / "name", name);
    for (const auto& sensor : sensors) {
      const std::string base = "temp" + std::to_string(sensor.index);
      writeText(hwmon / (base + "_input"), std::to_string(sensor.raw));
      if (!sensor.label.empty()) {
        writeText(hwmon / (base + "_label"), sensor.label);
      }
    }
    return hwmon;
  }

  void
  addThermalZone(const std::filesystem::path& thermalRoot, std::string_view dirName, std::string_view type, int raw) {
    const std::filesystem::path zone = thermalRoot / dirName;
    writeText(zone / "type", type);
    writeText(zone / "temp", std::to_string(raw));
  }

  noctalia::system::cpu_temp::ProbeResult
  readFixture(const std::filesystem::path& root, const std::string& configuredPath = "") {
    return noctalia::system::cpu_temp::read(root / "hwmon", root / "thermal", configuredPath);
  }

  void checkAmdPrefersTctl() {
    const auto root = makeTempDir();
    addHwmon(root / "hwmon", "hwmon3", "k10temp", {{1, "Tctl", 58000}, {3, "Tccd1", 48000}, {4, "Tccd2", 46000}});

    const auto result = readFixture(root);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    TEST_CHECK(result.reading.has_value());
    TEST_NEAR(result.reading->tempC, 58.0, 0.001);
    TEST_CHECK(result.reading->source.contains("Tctl"));
  }

  void checkMotherboardCpuLabelIsIgnored() {
    const auto root = makeTempDir();
    addHwmon(root / "hwmon", "hwmon0", "nct6798", {{11, "PCH_CPU_TEMP", 0}});
    addHwmon(root / "hwmon", "hwmon2", "k10temp", {{1, "Tctl", 58000}, {3, "Tccd1", 48000}});

    const auto result = readFixture(root);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    TEST_CHECK(result.reading.has_value());
    TEST_NEAR(result.reading->tempC, 58.0, 0.001);
    TEST_CHECK(result.reading->source.contains("k10temp"));
  }

  void checkIntelPackageWins() {
    const auto root = makeTempDir();
    addHwmon(root / "hwmon", "hwmon1", "coretemp", {{1, "Package id 0", 66000}, {2, "Core 0", 55000}});

    const auto result = readFixture(root);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    TEST_CHECK(result.reading.has_value());
    TEST_NEAR(result.reading->tempC, 66.0, 0.001);
    TEST_CHECK(result.reading->source.contains("Package id 0"));
  }

  void checkManualPathWins() {
    const auto root = makeTempDir();
    addHwmon(root / "hwmon", "hwmon2", "k10temp", {{1, "Tctl", 58000}});
    const auto manual = addHwmon(root / "hwmon", "hwmon5", "custom", {{2, "Manual Sensor", 42000}});

    const auto result = readFixture(root, (manual / "temp2_input").string());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    TEST_CHECK(result.reading.has_value());
    TEST_NEAR(result.reading->tempC, 42.0, 0.001);
    TEST_CHECK(result.reading->source.contains("configured"));
  }

  void checkManualInvalidDoesNotFallback() {
    const auto root = makeTempDir();
    addHwmon(root / "hwmon", "hwmon2", "k10temp", {{1, "Tctl", 58000}});

    const auto result = readFixture(root, (root / "hwmon" / "hwmon2" / "missing_input").string());
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    TEST_CHECK(!result.reading.has_value());
    TEST_CHECK(result.error.contains("temp*_input"));
  }

  void checkZeroAutoCandidateIsSkipped() {
    const auto root = makeTempDir();
    addHwmon(root / "hwmon", "hwmon2", "k10temp", {{1, "Tctl", 0}, {3, "Tccd1", 48000}});

    const auto result = readFixture(root);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    TEST_CHECK(result.reading.has_value());
    TEST_NEAR(result.reading->tempC, 48.0, 0.001);
    TEST_CHECK(result.reading->source.contains("Tccd1"));
  }

  void checkThermalFallback() {
    const auto root = makeTempDir();
    addHwmon(root / "hwmon", "hwmon0", "nct6798", {{11, "PCH_CPU_TEMP", 0}});
    addThermalZone(root / "thermal", "thermal_zone0", "x86_pkg_temp", 61000);

    const auto result = readFixture(root);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    TEST_CHECK(result.reading.has_value());
    TEST_NEAR(result.reading->tempC, 61.0, 0.001);
    TEST_CHECK(result.reading->source.contains("x86_pkg_temp"));
  }

} // namespace

int main() {
  checkAmdPrefersTctl();
  checkMotherboardCpuLabelIsIgnored();
  checkIntelPackageWins();
  checkManualPathWins();
  checkManualInvalidDoesNotFallback();
  checkZeroAutoCandidateIsSkipped();
  checkThermalFallback();
  return 0;
}
