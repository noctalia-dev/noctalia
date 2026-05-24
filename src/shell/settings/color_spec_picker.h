#pragma once

#include "ui/palette.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Select;

namespace settings {

  struct ColorSpecSelectOptions {
    std::vector<ColorRole> roles;
    std::string selectedValue;
    bool allowNone = false;
    bool allowCustomColor = true;
    std::string noneLabel;
    float fontSize = 0.0f;
    float controlHeight = 0.0f;
    float glyphSize = 0.0f;
    float width = 0.0f;
    bool flexGrow = false;
  };

  [[nodiscard]] std::vector<ColorRole> allColorSpecRoles();
  [[nodiscard]] std::string colorSpecConfigValue(const ColorSpec& color);
  [[nodiscard]] std::string optionalColorSpecConfigValue(const std::optional<ColorSpec>& color);

  [[nodiscard]] std::unique_ptr<Select> makeColorSpecSelect(
      ColorSpecSelectOptions options, std::function<void(std::string)> setValue, std::function<void()> clearValue
  );

} // namespace settings
