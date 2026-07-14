#pragma once

#include "ui/palette.h"

#include <string>
#include <string_view>

// Parses a user-facing color value: either a palette role token or a hex color.
[[nodiscard]] ColorSpec colorSpecFromConfigString(const std::string& raw, std::string_view context = {});

// Serializes a color spec to its stable config representation (palette role token or hex).
[[nodiscard]] std::string colorSpecToConfigString(const ColorSpec& spec);
