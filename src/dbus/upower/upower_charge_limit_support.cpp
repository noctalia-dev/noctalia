#include "dbus/upower/upower_charge_limit_support.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <ranges>
#include <string>

namespace {

  std::optional<std::uint32_t> readThresholdFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
      return std::nullopt;
    }

    std::string value;
    std::getline(input, value);
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      return std::nullopt;
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    const std::string_view trimmed(value.data() + first, last - first + 1);
    std::uint32_t threshold = 0;
    const auto [end, error] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), threshold);
    if (error != std::errc{} || end != trimmed.data() + trimmed.size() || threshold > 100U) {
      return std::nullopt;
    }
    return threshold;
  }

  bool isPlainPowerSupplyComponent(std::string_view value) {
    if (value.empty() || value == "." || value == "..") {
      return false;
    }
    return std::ranges::all_of(value, [](unsigned char ch) {
      return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.' || ch == ':';
    });
  }

  std::optional<std::string> powerSupplyComponent(std::string_view nativePath) {
    if (isPlainPowerSupplyComponent(nativePath)) {
      return std::string(nativePath);
    }

    const std::filesystem::path path{nativePath};
    if (!path.is_absolute()
        || !nativePath.starts_with("/sys/")
        || std::ranges::any_of(path, [](const std::filesystem::path& part) { return part == ".." || part == "."; })) {
      return std::nullopt;
    }

    const auto normalized = path.lexically_normal();
    const std::string component = normalized.filename().string();
    if (normalized.parent_path().filename() != "power_supply" || !isPlainPowerSupplyComponent(component)) {
      return std::nullopt;
    }
    return component;
  }

} // namespace

namespace upower::detail {

  ChargeThresholdProbe
  readChargeThresholdsFromSysfs(std::string_view nativePath, const std::filesystem::path& powerSupplyRoot) {
    ChargeThresholdProbe result;
    const auto component = powerSupplyComponent(nativePath);
    if (!component.has_value()) {
      return result;
    }

    // NativePath selects a kernel power_supply name only. Always read through the fixed class root,
    // even when UPower supplies its documented absolute /sys/devices/.../power_supply path.
    const auto batteryPath = powerSupplyRoot / *component;
    result.start = readThresholdFile(batteryPath / "charge_control_start_threshold");
    result.end = readThresholdFile(batteryPath / "charge_control_end_threshold");
    return result;
  }

} // namespace upower::detail
