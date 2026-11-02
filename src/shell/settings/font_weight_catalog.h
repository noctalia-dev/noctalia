#pragma once

#include "shell/settings/widget_settings_registry.h"

#include <optional>
#include <string_view>
#include <vector>

namespace settings {

  enum class FontWeightSelectKind : std::uint8_t {
    WidgetInheritDefault,
    BarDefault,
  };

  std::vector<WidgetSettingSelectOption> buildLabelFontWeightSelectOptions(
      std::string_view fontFamily, FontWeightSelectKind kind, std::optional<int> preserveConfiguredWeight = std::nullopt
  );

} // namespace settings
