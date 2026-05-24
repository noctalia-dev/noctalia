#include "shell/settings/color_spec_picker.h"

#include "i18n/i18n.h"
#include "ui/builders.h"
#include "ui/dialogs/color_picker_dialog.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string_view>
#include <utility>

namespace settings {
  namespace {

    constexpr std::string_view kCustomColorValue = "__custom_color__";

    struct Choice {
      std::string value;
      std::string label;
    };

    std::vector<std::string> labelsForChoices(const std::vector<Choice>& choices) {
      std::vector<std::string> labels;
      labels.reserve(choices.size());
      for (const auto& choice : choices) {
        labels.push_back(choice.label);
      }
      return labels;
    }

    std::optional<std::size_t> choiceIndex(const std::vector<Choice>& choices, std::string_view value) {
      for (std::size_t i = 0; i < choices.size(); ++i) {
        if (choices[i].value == value) {
          return i;
        }
      }
      return std::nullopt;
    }

    int colorByteForConfig(float value) {
      return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    }

    std::string formatFixedColorConfigValue(const Color& color) {
      if (color.a >= 0.999f) {
        return formatRgbHex(color);
      }

      char buffer[16];
      std::snprintf(
          buffer, sizeof(buffer), "#%02X%02X%02X%02X", colorByteForConfig(color.r), colorByteForConfig(color.g),
          colorByteForConfig(color.b), colorByteForConfig(color.a)
      );
      return std::string(buffer);
    }

  } // namespace

  std::vector<ColorRole> allColorSpecRoles() {
    std::vector<ColorRole> roles;
    roles.reserve(kColorRoleTokens.size());
    for (const auto& token : kColorRoleTokens) {
      roles.push_back(token.role);
    }
    return roles;
  }

  std::string colorSpecConfigValue(const ColorSpec& color) {
    if (color.role.has_value()) {
      return std::string(colorRoleToken(*color.role));
    }
    Color fixed = color.fixed;
    fixed.a *= color.alpha;
    return formatFixedColorConfigValue(fixed);
  }

  std::string optionalColorSpecConfigValue(const std::optional<ColorSpec>& color) {
    return color.has_value() ? colorSpecConfigValue(*color) : std::string{};
  }

  std::unique_ptr<Select> makeColorSpecSelect(
      ColorSpecSelectOptions options, std::function<void(std::string)> setValue, std::function<void()> clearValue
  ) {
    const std::vector<ColorRole> roles = options.roles.empty() ? allColorSpecRoles() : std::move(options.roles);

    std::vector<Choice> choices;
    std::vector<ColorSpec> indicators;
    choices.reserve(roles.size() + (options.allowNone ? 1U : 0U) + (options.allowCustomColor ? 1U : 0U));
    indicators.reserve(choices.capacity());

    if (options.allowNone) {
      choices.push_back(
          Choice{
              .value = {},
              .label = options.noneLabel.empty() ? i18n::tr("settings.options.theme-role.default") : options.noneLabel,
          }
      );
      indicators.push_back(clearColorSpec());
    }

    for (const auto role : roles) {
      choices.push_back(Choice{.value = std::string(colorRoleToken(role)), .label = std::string(colorRoleToken(role))});
      indicators.push_back(colorSpecFromRole(role));
    }

    Color selectedFixedColor;
    const bool selectedIsFixedColor =
        options.allowCustomColor && tryParseHexColor(options.selectedValue, selectedFixedColor);
    if (options.allowCustomColor) {
      choices.push_back(
          Choice{
              .value = std::string(kCustomColorValue),
              .label = selectedIsFixedColor ? formatFixedColorConfigValue(selectedFixedColor)
                                            : i18n::tr("settings.options.theme-role.custom"),
          }
      );
      indicators.push_back(selectedIsFixedColor ? fixedColorSpec(selectedFixedColor) : clearColorSpec());
    }

    std::optional<Color> customInitialColor;
    std::string selectedValue = options.selectedValue;
    if (selectedIsFixedColor) {
      customInitialColor = selectedFixedColor;
      selectedValue = std::string(kCustomColorValue);
    } else if (const auto selectedRole = colorRoleFromToken(selectedValue); selectedRole.has_value()) {
      customInitialColor = colorForRole(*selectedRole);
    }

    const auto selectedIndex = choiceIndex(choices, selectedValue);
    const bool selectedUnknown = !selectedIndex.has_value() && !selectedValue.empty();

    auto select = ui::select({
        .options = labelsForChoices(choices),
        .selectedIndex = selectedIndex,
        .clearSelection = selectedUnknown,
        .placeholder =
            selectedUnknown
                ? std::optional<std::string>(i18n::tr("settings.controls.select.unknown-value", "value", selectedValue))
                : std::nullopt,
        .fontSize = options.fontSize > 0.0f ? std::optional<float>(options.fontSize) : std::nullopt,
        .controlHeight = options.controlHeight > 0.0f ? std::optional<float>(options.controlHeight) : std::nullopt,
        .glyphSize = options.glyphSize > 0.0f ? std::optional<float>(options.glyphSize) : std::nullopt,
        .optionIndicators = std::move(indicators),
        .width = options.width > 0.0f ? std::optional<float>(options.width) : std::nullopt,
        .height =
            options.width > 0.0f
                ? std::optional<float>(options.controlHeight > 0.0f ? options.controlHeight : Style::controlHeight)
                : std::nullopt,
        .flexGrow = options.flexGrow ? std::optional<float>(1.0f) : std::nullopt,
        .onSelectionChanged =
            [choices = std::move(choices), setValue = std::move(setValue), clearValue = std::move(clearValue),
             initialColor = customInitialColor](std::size_t index, std::string_view /*label*/) mutable {
              if (index >= choices.size()) {
                return;
              }
              if (choices[index].value == kCustomColorValue) {
                ColorPickerDialogOptions dialogOptions;
                dialogOptions.title = i18n::tr("settings.dialogs.color-picker.title");
                if (initialColor.has_value()) {
                  dialogOptions.initialColor = *initialColor;
                } else if (const auto last = ColorPickerDialog::lastResult()) {
                  dialogOptions.initialColor = *last;
                }
                (void)ColorPickerDialog::open(std::move(dialogOptions), [setValue](std::optional<Color> result) {
                  if (!result.has_value()) {
                    return;
                  }
                  Color rgb = *result;
                  rgb.a = 1.0f;
                  setValue(formatFixedColorConfigValue(rgb));
                });
                return;
              }
              if (choices[index].value.empty()) {
                clearValue();
                return;
              }
              setValue(choices[index].value);
            },
    });

    return select;
  }

} // namespace settings
