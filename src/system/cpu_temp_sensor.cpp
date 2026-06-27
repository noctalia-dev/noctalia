#include "system/cpu_temp_sensor.h"

#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace noctalia::system::cpu_temp {
  namespace {

    struct Sensor {
      std::string hwmonName;
      std::string label;
      std::filesystem::path inputPath;
      double tempC = 0.0;
      int driverPriority = 100;
      int sensorPriority = 100;
    };

    [[nodiscard]] std::optional<double> readInputCelsius(const std::filesystem::path& path) {
      std::ifstream file{path};
      if (!file.is_open()) {
        return std::nullopt;
      }

      long long raw = 0;
      file >> raw;
      if (file.fail() || raw < 0) {
        return std::nullopt;
      }

      if (raw >= 1000) {
        return static_cast<double>(raw) / 1000.0;
      }
      return static_cast<double>(raw);
    }

    [[nodiscard]] bool isTempInputFileName(const std::string& fileName) {
      constexpr std::string_view prefix = "temp";
      constexpr std::string_view suffix = "_input";
      if (!fileName.starts_with(prefix) || !fileName.ends_with(suffix)) {
        return false;
      }
      const std::string_view number{fileName.data() + prefix.size(), fileName.size() - prefix.size() - suffix.size()};
      return !number.empty() && std::ranges::all_of(number, [](char ch) { return ch >= '0' && ch <= '9'; });
    }

    [[nodiscard]] int tempInputIndex(const std::string& fileName) {
      if (!isTempInputFileName(fileName)) {
        return 0;
      }

      constexpr std::string_view prefix = "temp";
      constexpr std::string_view suffix = "_input";
      const std::string_view number{fileName.data() + prefix.size(), fileName.size() - prefix.size() - suffix.size()};
      int index = 0;
      const auto [ptr, ec] = std::from_chars(number.data(), number.data() + number.size(), index);
      if (ec != std::errc{} || ptr != number.data() + number.size()) {
        return 0;
      }
      return index;
    }

    [[nodiscard]] std::string labelForInputPath(const std::filesystem::path& inputPath) {
      const std::string fileName = inputPath.filename().string();
      const int index = tempInputIndex(fileName);
      const std::string base = fileName.substr(0, fileName.size() - std::string{"_input"}.size());
      return FileUtils::readSmallTextFile(inputPath.parent_path() / (base + "_label"))
          .value_or("temp" + std::to_string(index));
    }

    [[nodiscard]] std::string
    formatHwmonSource(const std::string& hwmonName, const std::string& label, const std::filesystem::path& inputPath) {
      const std::string name = hwmonName.empty() ? "unknown" : hwmonName;
      if (label.empty()) {
        return std::format("hwmon:{} {}", name, inputPath.string());
      }
      return std::format("hwmon:{} label=\"{}\" {}", name, label, inputPath.string());
    }

    [[nodiscard]] std::string formatThermalSource(const std::string& zoneType, const std::filesystem::path& inputPath) {
      const std::string type = zoneType.empty() ? "unknown" : zoneType;
      return std::format("thermal_zone:{} {}", type, inputPath.string());
    }

    [[nodiscard]] bool labelStartsWith(const std::string& label, std::string_view prefix) {
      return label.starts_with(prefix);
    }

    [[nodiscard]] int knownDriverPriority(const std::string& hwmonName) {
      const std::string name = StringUtils::toLower(hwmonName);
      if (name == "k10temp") {
        return 0;
      }
      if (name == "zenpower") {
        return 1;
      }
      if (name == "coretemp") {
        return 2;
      }
      if (name == "ibmpowernv") {
        return 3;
      }
      return -1;
    }

    [[nodiscard]] int sensorPriorityForDriver(const std::string& hwmonName, const std::string& label, int inputIndex) {
      const std::string name = StringUtils::toLower(hwmonName);
      const std::string lowerLabel = StringUtils::toLower(label);

      if (name == "k10temp" || name == "zenpower") {
        if (labelStartsWith(label, "Tctl")) {
          return 0;
        }
        if (inputIndex == 1) {
          return 1;
        }
        if (labelStartsWith(label, "Tdie")) {
          return 2;
        }
        if (labelStartsWith(label, "Package id")) {
          return 3;
        }
        if (labelStartsWith(label, "SoC Temperature")) {
          return 4;
        }
        if (labelStartsWith(label, "Core") || labelStartsWith(label, "Tccd")) {
          return 5;
        }
        return 20;
      }

      if (name == "coretemp") {
        if (labelStartsWith(label, "Package id")) {
          return 0;
        }
        if (inputIndex == 1) {
          return 1;
        }
        if (labelStartsWith(label, "Core")) {
          return 2;
        }
        return 20;
      }

      if (name == "ibmpowernv") {
        if (lowerLabel.contains("core")) {
          return 0;
        }
        if (inputIndex == 1) {
          return 1;
        }
        return 20;
      }

      return 100;
    }

    [[nodiscard]] std::vector<std::filesystem::path> sortedDirectories(const std::filesystem::path& root) {
      namespace fs = std::filesystem;

      std::vector<fs::path> paths;
      std::error_code ec;
      if (!fs::is_directory(root, ec)) {
        return paths;
      }

      for (const auto& entry : fs::directory_iterator{root, ec}) {
        if (entry.is_directory(ec)) {
          paths.push_back(entry.path());
        }
      }
      std::ranges::sort(paths);
      return paths;
    }

    [[nodiscard]] std::vector<Sensor> readKnownHwmonSensors(const std::filesystem::path& hwmonRoot) {
      namespace fs = std::filesystem;

      std::vector<Sensor> sensors;
      for (const auto& hwmonPath : sortedDirectories(hwmonRoot)) {
        const std::string hwmonName =
            FileUtils::readSmallTextFile(hwmonPath / "name").value_or(hwmonPath.filename().string());
        const int driverPriority = knownDriverPriority(hwmonName);
        if (driverPriority < 0) {
          continue;
        }

        std::vector<fs::path> inputPaths;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator{hwmonPath, ec}) {
          const std::string fileName = entry.path().filename().string();
          if (isTempInputFileName(fileName)) {
            inputPaths.push_back(entry.path());
          }
        }
        std::ranges::sort(inputPaths);

        for (const auto& inputPath : inputPaths) {
          const auto tempC = readInputCelsius(inputPath);
          if (!tempC.has_value()) {
            continue;
          }

          const std::string fileName = inputPath.filename().string();
          const int inputIndex = tempInputIndex(fileName);
          const std::string label = labelForInputPath(inputPath);
          sensors.push_back(
              Sensor{
                  .hwmonName = hwmonName,
                  .label = label,
                  .inputPath = inputPath,
                  .tempC = *tempC,
                  .driverPriority = driverPriority,
                  .sensorPriority = sensorPriorityForDriver(hwmonName, label, inputIndex),
              }
          );
        }
      }
      return sensors;
    }

    [[nodiscard]] std::optional<Reading> chooseKnownHwmonSensor(const std::filesystem::path& hwmonRoot) {
      std::vector<Sensor> sensors = readKnownHwmonSensors(hwmonRoot);
      if (sensors.empty()) {
        return std::nullopt;
      }

      const bool hasNonZero = std::ranges::any_of(sensors, [](const Sensor& sensor) { return sensor.tempC > 0.0; });
      if (hasNonZero) {
        std::erase_if(sensors, [](const Sensor& sensor) { return sensor.tempC <= 0.0; });
      }

      const auto best = std::ranges::min_element(sensors, [](const Sensor& lhs, const Sensor& rhs) {
        if (lhs.driverPriority != rhs.driverPriority) {
          return lhs.driverPriority < rhs.driverPriority;
        }
        if (lhs.sensorPriority != rhs.sensorPriority) {
          return lhs.sensorPriority < rhs.sensorPriority;
        }
        return lhs.inputPath.string() < rhs.inputPath.string();
      });
      if (best == sensors.end()) {
        return std::nullopt;
      }

      return Reading{.tempC = best->tempC, .source = formatHwmonSource(best->hwmonName, best->label, best->inputPath)};
    }

    [[nodiscard]] int knownThermalZonePriority(const std::string& zoneType) {
      if (zoneType == "cpu-thermal") {
        return 0;
      }
      if (zoneType == "x86_pkg_temp") {
        return 1;
      }
      if (zoneType == "acpitz") {
        return 2;
      }
      return -1;
    }

    [[nodiscard]] std::optional<Reading> chooseThermalZoneSensor(const std::filesystem::path& thermalRoot) {
      struct ThermalSensor {
        std::string type;
        std::filesystem::path inputPath;
        double tempC = 0.0;
        int priority = 100;
      };

      std::vector<ThermalSensor> sensors;
      for (const auto& zonePath : sortedDirectories(thermalRoot)) {
        const std::string type = FileUtils::readSmallTextFile(zonePath / "type").value_or("");
        const int priority = knownThermalZonePriority(type);
        if (priority < 0) {
          continue;
        }

        const std::filesystem::path tempPath = zonePath / "temp";
        const auto tempC = readInputCelsius(tempPath);
        if (!tempC.has_value()) {
          continue;
        }
        sensors.push_back(ThermalSensor{.type = type, .inputPath = tempPath, .tempC = *tempC, .priority = priority});
      }

      if (sensors.empty()) {
        return std::nullopt;
      }

      const bool hasNonZero =
          std::ranges::any_of(sensors, [](const ThermalSensor& sensor) { return sensor.tempC > 0.0; });
      if (hasNonZero) {
        std::erase_if(sensors, [](const ThermalSensor& sensor) { return sensor.tempC <= 0.0; });
      }

      const auto best = std::ranges::min_element(sensors, [](const ThermalSensor& lhs, const ThermalSensor& rhs) {
        if (lhs.priority != rhs.priority) {
          return lhs.priority < rhs.priority;
        }
        return lhs.inputPath.string() < rhs.inputPath.string();
      });
      if (best == sensors.end()) {
        return std::nullopt;
      }

      return Reading{.tempC = best->tempC, .source = formatThermalSource(best->type, best->inputPath)};
    }

    [[nodiscard]] ProbeResult readConfiguredSensor(const std::filesystem::path& configuredPath) {
      namespace fs = std::filesystem;

      const std::string fileName = configuredPath.filename().string();
      if (!isTempInputFileName(fileName)) {
        return ProbeResult{
            .reading = std::nullopt,
            .error =
                std::format("configured CPU temperature sensor is not a temp*_input file: {}", configuredPath.string())
        };
      }

      std::error_code ec;
      if (!fs::exists(configuredPath, ec) || ec) {
        return ProbeResult{
            .reading = std::nullopt,
            .error = std::format("configured CPU temperature sensor does not exist: {}", configuredPath.string())
        };
      }
      if (!fs::is_regular_file(configuredPath, ec) || ec) {
        return ProbeResult{
            .reading = std::nullopt,
            .error = std::format(
                "configured CPU temperature sensor is not readable as a regular file: {}", configuredPath.string()
            )
        };
      }

      const auto tempC = readInputCelsius(configuredPath);
      if (!tempC.has_value()) {
        return ProbeResult{
            .reading = std::nullopt,
            .error = std::format("configured CPU temperature sensor could not be parsed: {}", configuredPath.string())
        };
      }

      const std::string label = labelForInputPath(configuredPath);
      const std::string hwmonName = FileUtils::readSmallTextFile(configuredPath.parent_path() / "name").value_or("");
      return ProbeResult{
          .reading =
              Reading{
                  .tempC = *tempC,
                  .source = "configured " + formatHwmonSource(hwmonName, label, configuredPath),
              },
          .error = {}
      };
    }

  } // namespace

  ProbeResult read(
      const std::filesystem::path& hwmonRoot, const std::filesystem::path& thermalRoot,
      const std::string& configuredSensorPath
  ) {
    if (!configuredSensorPath.empty()) {
      return readConfiguredSensor(std::filesystem::path{configuredSensorPath});
    }

    if (auto hwmon = chooseKnownHwmonSensor(hwmonRoot); hwmon.has_value()) {
      return ProbeResult{.reading = std::move(hwmon), .error = {}};
    }

    if (auto thermal = chooseThermalZoneSensor(thermalRoot); thermal.has_value()) {
      return ProbeResult{.reading = std::move(thermal), .error = {}};
    }

    return ProbeResult{.reading = std::nullopt, .error = "no CPU temperature sensor found"};
  }

} // namespace noctalia::system::cpu_temp
