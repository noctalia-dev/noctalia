#include "system/cpu_temp_sensor.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

  int g_failures = 0;

  void fail(const std::string& message) {
    std::fprintf(stderr, "cpu_temp_sensor_test: FAIL: %s\n", message.c_str());
    ++g_failures;
  }

  bool expect(bool condition, const std::string& message) {
    if (!condition) {
      fail(message);
      return false;
    }
    return true;
  }

  bool expectTemp(const noctalia::system::cpu_temp::ProbeResult& result, double expected, const std::string& message) {
    if (!result.reading.has_value()) {
      fail(message + ": no reading; error=" + result.error);
      return false;
    }
    if (std::abs(result.reading->tempC - expected) > 0.001) {
      fail(message + ": expected " + std::to_string(expected) + ", got " + std::to_string(result.reading->tempC));
      return false;
    }
    return true;
  }

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
    expectTemp(result, 58.0, "AMD k10temp should prefer Tctl");
    expect(
        result.reading.has_value() && result.reading->source.find("Tctl") != std::string::npos,
        "AMD k10temp source should name Tctl"
    );
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }

  void checkMotherboardCpuLabelIsIgnored() {
    const auto root = makeTempDir();
    addHwmon(root / "hwmon", "hwmon0", "nct6798", {{11, "PCH_CPU_TEMP", 0}});
    addHwmon(root / "hwmon", "hwmon2", "k10temp", {{1, "Tctl", 58000}, {3, "Tccd1", 48000}});

    const auto result = readFixture(root);
    expectTemp(result, 58.0, "known CPU hwmon should beat motherboard CPU-labeled sensor");
    expect(
        result.reading.has_value() && result.reading->source.find("k10temp") != std::string::npos,
        "source should be k10temp"
    );
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }

  void checkIntelPackageWins() {
    const auto root = makeTempDir();
    addHwmon(root / "hwmon", "hwmon1", "coretemp", {{1, "Package id 0", 66000}, {2, "Core 0", 55000}});

    const auto result = readFixture(root);
    expectTemp(result, 66.0, "Intel coretemp should prefer package sensor");
    expect(
        result.reading.has_value() && result.reading->source.find("Package id 0") != std::string::npos,
        "Intel source should name package label"
    );
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }

  void checkManualPathWins() {
    const auto root = makeTempDir();
    addHwmon(root / "hwmon", "hwmon2", "k10temp", {{1, "Tctl", 58000}});
    const auto manual = addHwmon(root / "hwmon", "hwmon5", "custom", {{2, "Manual Sensor", 42000}});

    const auto result = readFixture(root, (manual / "temp2_input").string());
    expectTemp(result, 42.0, "configured sensor path should win over auto detection");
    expect(
        result.reading.has_value() && result.reading->source.find("configured") != std::string::npos,
        "manual source should be marked configured"
    );
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }

  void checkManualInvalidDoesNotFallback() {
    const auto root = makeTempDir();
    addHwmon(root / "hwmon", "hwmon2", "k10temp", {{1, "Tctl", 58000}});

    const auto result = readFixture(root, (root / "hwmon" / "hwmon2" / "missing_input").string());
    expect(!result.reading.has_value(), "invalid configured sensor should not fall back to auto");
    expect(result.error.find("temp*_input") != std::string::npos, "invalid configured path should explain filename");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }

  void checkZeroAutoCandidateIsSkipped() {
    const auto root = makeTempDir();
    addHwmon(root / "hwmon", "hwmon2", "k10temp", {{1, "Tctl", 0}, {3, "Tccd1", 48000}});

    const auto result = readFixture(root);
    expectTemp(result, 48.0, "zero auto candidate should be skipped when a non-zero known CPU sensor exists");
    expect(
        result.reading.has_value() && result.reading->source.find("Tccd1") != std::string::npos,
        "non-zero source should be selected"
    );
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }

  void checkThermalFallback() {
    const auto root = makeTempDir();
    addHwmon(root / "hwmon", "hwmon0", "nct6798", {{11, "PCH_CPU_TEMP", 0}});
    addThermalZone(root / "thermal", "thermal_zone0", "x86_pkg_temp", 61000);

    const auto result = readFixture(root);
    expectTemp(result, 61.0, "known thermal zone should be used when no known CPU hwmon is available");
    expect(
        result.reading.has_value() && result.reading->source.find("x86_pkg_temp") != std::string::npos,
        "thermal fallback source should name zone type"
    );
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
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

  if (g_failures == 0) {
    std::puts("cpu_temp_sensor_test: all checks passed");
    return 0;
  }
  std::fprintf(stderr, "cpu_temp_sensor_test: %d failure(s)\n", g_failures);
  return 1;
}
