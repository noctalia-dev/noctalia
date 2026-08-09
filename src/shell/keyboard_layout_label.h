#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

enum class KeyboardLayoutDisplayMode : std::uint8_t { Short = 0, Full = 1 };

[[nodiscard]] KeyboardLayoutDisplayMode parseKeyboardLayoutDisplayMode(std::string_view value);
[[nodiscard]] std::string
formatKeyboardLayoutLabel(const std::string& layoutName, KeyboardLayoutDisplayMode displayMode);
[[nodiscard]] std::string resolveKeyboardLayoutLabel(
    const std::string& layoutName, KeyboardLayoutDisplayMode displayMode,
    const std::unordered_map<std::string, std::string>& customLabels
);
