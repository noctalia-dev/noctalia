#pragma once

#include <cstdint>

// Navigation and editing actions shared by UI controls; apps bind them to key chords.
enum class KeybindAction : std::uint8_t {
  Validate = 0,
  Cancel = 1,
  Left = 2,
  Right = 3,
  Up = 4,
  Down = 5,
  TabNext = 6,
  TabPrevious = 7,
  Delete = 8,
  Copy = 9,
  Save = 10,
  SelectAll = 11,
  CategoryNext = 12,
  CategoryPrevious = 13,
};
