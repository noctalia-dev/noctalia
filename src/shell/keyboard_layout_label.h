#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

enum class KeyboardLayoutDisplayMode : std::uint8_t { Short = 0, Full = 1 };

KeyboardLayoutDisplayMode parseKeyboardLayoutDisplayMode(std::string_view value);
std::string formatKeyboardLayoutLabel(const std::string& layoutName, KeyboardLayoutDisplayMode displayMode);
std::string resolveKeyboardLayoutLabel(
    const std::string& layoutName, KeyboardLayoutDisplayMode displayMode,
    const std::unordered_map<std::string, std::string>& customLabels
);
// Like resolveKeyboardLayoutLabel, but keeps an unrecognized name (e.g. "Mozc") instead of returning "--".
std::string resolveInputMethodLabel(
    const std::string& inputMethodName, KeyboardLayoutDisplayMode displayMode,
    const std::unordered_map<std::string, std::string>& customLabels
);
