#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace compositors::hyprland {

  std::string formatWindowAddress(std::uint64_t address);

  // Lowercase hex without 0x prefix; empty when invalid.
  std::string normalizeWindowId(std::string_view value);

  bool windowIdsEqual(std::string_view lhs, std::string_view rhs);

  std::optional<std::uint64_t> parseWindowAddress(std::string_view value);

} // namespace compositors::hyprland
