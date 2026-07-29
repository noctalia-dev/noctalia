#pragma once

#include "ui/controls/color_swatch_preview.h"
#include "ui/palette.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class SelectPopupContext {
public:
  virtual ~SelectPopupContext() = default;

  struct DropdownRequest {
    std::int32_t anchorX = 0;
    std::int32_t anchorY = 0;
    std::int32_t anchorWidth = 1;
    std::int32_t anchorHeight = 1;
    float menuWidth = 0.0f;
    // When > menuWidth, the dropdown grows past menuWidth to fit its widest option, up to this cap.
    float maxMenuWidth = 0.0f;
    // Drives the dropdown's row metrics (font, height, insets); pre-scaled by the caller.
    float fontSize = 0.0f;
    std::vector<std::string> options;
    std::vector<ColorSpec> indicatorColors;
    std::vector<ColorSwatchPreview> optionSwatchPreviews;
    std::size_t selectedIndex = static_cast<std::size_t>(-1);
    std::size_t maxVisibleOptions = 6;
  };

  struct DropdownCallbacks {
    std::function<void(std::size_t index)> onSelect;
    std::function<void()> onDismiss;
  };

  virtual void openSelectDropdown(const DropdownRequest& request, DropdownCallbacks callbacks) = 0;
  virtual void closeSelectDropdown() = 0;
  [[nodiscard]] virtual bool isSelectDropdownOpen() const = 0;
};
