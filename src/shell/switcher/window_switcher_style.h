#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

enum class WindowSwitcherTileDepth : std::uint8_t {
  Selected,
  Near,
  Far,
};

enum class WindowSwitcherHorizontalPlacement : std::uint8_t {
  Left,
  Right,
};

struct WindowSwitcherStyleContext {
  float screenWidth = 0.0F;
  float screenHeight = 0.0F;
  float scale = 1.0F;
  std::size_t windowCount = 0;
  std::size_t selectedIndex = 0;
  bool showCaption = true;
  bool showCount = true;
};

struct WindowSwitcherCardTarget {
  bool visible = false;
  bool showCaption = false;
  bool wideCaption = false;
  WindowSwitcherHorizontalPlacement iconPlacement = WindowSwitcherHorizontalPlacement::Left;
  WindowSwitcherHorizontalPlacement closePlacement = WindowSwitcherHorizontalPlacement::Right;
  WindowSwitcherTileDepth depth = WindowSwitcherTileDepth::Far;
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
  float opacity = 0.0F;
  float direction = 0.0F;
  std::int32_t zIndex = 0;
};

struct WindowSwitcherStyleLayout {
  std::vector<WindowSwitcherCardTarget> cards;
  std::size_t visibleCards = 0;
  float panelWidth = 0.0F;
  float panelHeight = 0.0F;
  float stripX = 0.0F;
  float stripY = 0.0F;
  float stripWidth = 0.0F;
  float stripHeight = 0.0F;
  float countX = 0.0F;
  float countY = 0.0F;
  float countWidth = 0.0F;
  float countHeight = 0.0F;
  bool boxed = false;

  [[nodiscard]] bool sameGeometryAs(const WindowSwitcherStyleLayout& other) const noexcept;
};
