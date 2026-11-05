#pragma once

#include <cstdint>

namespace KeyMod {
  inline constexpr std::uint32_t Shift = 1U << 0;
  inline constexpr std::uint32_t Ctrl = 1U << 1;
  inline constexpr std::uint32_t Alt = 1U << 2;
  inline constexpr std::uint32_t Super = 1U << 3;
} // namespace KeyMod
