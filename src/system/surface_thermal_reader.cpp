#include "system/surface_thermal_reader.h"

#include "util/file_utils.h"

#include <charconv>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace noctalia::system::surface {
  namespace {

    [[nodiscard]] std::string trimTrailing(std::string value) {
      while (!value.empty()
             && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
      }
      return value;
    }

    [[nodiscard]] std::optional<std::string> hwmonName(const std::filesystem::path& dir) {
      auto name = FileUtils::readSmallTextFile(dir / "name");
      if (!name.has_value()) {
        return std::nullopt;
      }
      return trimTrailing(std::move(*name));
    }

    [[nodiscard]] std::optional<std::uint32_t> readU32(const std::filesystem::path& path) {
      const auto text = FileUtils::readSmallTextFile(path);
      if (!text.has_value()) {
        return std::nullopt;
      }
      const auto trimmed = trimTrailing(*text);
      std::uint32_t value = 0;
      const auto [end, error] =
          std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), value);
      if (error != std::errc{} || end != trimmed.data() + trimmed.size()) {
        return std::nullopt;
      }
      return value;
    }

    [[nodiscard]] std::optional<double> readTempCelsius(const std::filesystem::path& path) {
      const auto raw = readU32(path);
      if (!raw.has_value()) {
        return std::nullopt;
      }
      if (*raw >= 1000U) {
        return static_cast<double>(*raw) / 1000.0;
      }
      return static_cast<double>(*raw);
    }

    [[nodiscard]] bool isTempInputName(std::string_view fileName) {
      return fileName.starts_with("temp") && fileName.ends_with("_input");
    }

  } // namespace

  ThermalSnapshot readThermal(const std::filesystem::path& hwmonRoot) {
    ThermalSnapshot snapshot;
    std::error_code error;
    if (!std::filesystem::is_directory(hwmonRoot, error) || error) {
      return snapshot;
    }

    for (const auto& entry : std::filesystem::directory_iterator(hwmonRoot, error)) {
      if (error || !entry.is_directory(error) || error) {
        continue;
      }
      const auto name = hwmonName(entry.path());
      if (!name.has_value()) {
        continue;
      }

      if (*name == "surface_fan" && !snapshot.fan.has_value()) {
        if (const auto rpm = readU32(entry.path() / "fan1_input")) {
          snapshot.fan = FanReading{.rpm = *rpm};
        }
        continue;
      }

      if (*name != "surface_thermal") {
        continue;
      }

      for (const auto& file : std::filesystem::directory_iterator(entry.path(), error)) {
        if (error || !file.is_regular_file(error) || error) {
          continue;
        }
        const auto fileName = file.path().filename().string();
        if (!isTempInputName(fileName)) {
          continue;
        }
        const auto celsius = readTempCelsius(file.path());
        if (!celsius.has_value()) {
          continue;
        }
        const auto base = fileName.substr(0, fileName.size() - std::string{"_input"}.size());
        auto label = FileUtils::readSmallTextFile(entry.path() / (base + "_label"));
        TempReading reading;
        reading.label = label.has_value() ? trimTrailing(std::move(*label)) : base;
        reading.celsius = *celsius;
        snapshot.temperatures.push_back(std::move(reading));
      }
    }

    return snapshot;
  }

} // namespace noctalia::system::surface
