#pragma once

#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace desktop_widgets {

  inline constexpr std::string_view kHardwareOutputKeyPrefix = "hardware:v1:";

  [[nodiscard]] inline std::string canonicalOutputIdentityField(std::string_view value) {
    return StringUtils::windowTitleSingleLine(StringUtils::trim(value));
  }

  [[nodiscard]] inline bool hasReliableOutputSerial(std::string_view serialNumber) {
    const std::string serial = canonicalOutputIdentityField(serialNumber);
    if (serial.empty()) {
      return false;
    }

    std::string placeholder;
    placeholder.reserve(serial.size());
    for (const unsigned char ch : serial) {
      if (std::isalnum(ch) != 0) {
        placeholder.push_back(static_cast<char>(std::tolower(ch)));
      }
    }

    constexpr std::array<std::string_view, 8> kPlaceholders = {
        "na", "none", "notavailable", "notspecified", "null", "unknown", "unspecified", "serialnumber",
    };
    const bool allZeros = std::ranges::all_of(placeholder, [](char ch) { return ch == '0'; });
    return !placeholder.empty() && !allZeros && !std::ranges::contains(kPlaceholders, placeholder);
  }

  [[nodiscard]] inline std::string hardwareOutputKey(const WaylandOutput& output) {
    if (!hasReliableOutputSerial(output.serialNumber)) {
      return {};
    }

    return std::string(kHardwareOutputKeyPrefix)
        + StringUtils::urlEncode(canonicalOutputIdentityField(output.make))
        + '/'
        + StringUtils::urlEncode(canonicalOutputIdentityField(output.model))
        + '/'
        + StringUtils::urlEncode(canonicalOutputIdentityField(output.serialNumber));
  }

  [[nodiscard]] inline std::string placementOutputKey(const WaylandOutput& output) {
    if (std::string hardwareKey = hardwareOutputKey(output); !hardwareKey.empty()) {
      return hardwareKey;
    }
    if (!output.connectorName.empty()) {
      return output.connectorName;
    }
    return std::to_string(output.name);
  }

  [[nodiscard]] inline bool outputMatchesPlacementKey(std::string_view key, const WaylandOutput& output) {
    if (key.empty()) {
      return false;
    }
    if (!output.connectorName.empty() && key == output.connectorName) {
      return true;
    }
    const std::string hardwareKey = hardwareOutputKey(output);
    return !hardwareKey.empty() && key == hardwareKey;
  }

  [[nodiscard]] inline const std::string& connectorNameForOutputApi(const WaylandOutput& output) noexcept {
    return output.connectorName;
  }

} // namespace desktop_widgets
