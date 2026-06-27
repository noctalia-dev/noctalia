#include "shell/settings/font_weight_catalog.h"

#include "render/text/font_weight_catalog.h"
#include "shell/settings/font_weight_i18n.h"

#include <algorithm>
#include <string>

namespace settings {
  namespace {

    [[nodiscard]] const char* fontWeightLabelKey(FontWeight weight) {
      for (const FontWeightI18nOption& option : kFontWeightOptions) {
        if (option.weight == weight) {
          return option.labelKey.data();
        }
      }
      return "settings.options.font-weight.regular";
    }

    [[nodiscard]] bool vectorContainsWeight(const std::vector<FontWeight>& weights, int weightValue) {
      return std::ranges::any_of(weights, [weightValue](FontWeight weight) {
        return static_cast<int>(weight) == weightValue;
      });
    }

  } // namespace

  std::vector<WidgetSettingSelectOption> buildLabelFontWeightSelectOptions(
      std::string_view fontFamily, FontWeightSelectKind kind, std::optional<int> preserveConfiguredWeight
  ) {
    const std::vector<FontWeight> available = text::availableLabelFontWeights(fontFamily);

    std::vector<WidgetSettingSelectOption> options;
    options.reserve(available.size() + 2);

    if (kind == FontWeightSelectKind::WidgetInheritDefault) {
      options.push_back(WidgetSettingSelectOption{"", "settings.options.font-weight.default"});
    }

    auto appendOption = [&](FontWeight weight) {
      options.push_back(
          WidgetSettingSelectOption{
              std::to_string(static_cast<int>(weight)),
              std::string(fontWeightLabelKey(weight)),
          }
      );
    };

    for (const FontWeight weight : available) {
      appendOption(weight);
    }

    if (preserveConfiguredWeight.has_value() && !vectorContainsWeight(available, *preserveConfiguredWeight)) {
      for (const FontWeightI18nOption& option : kFontWeightOptions) {
        if (static_cast<int>(option.weight) == *preserveConfiguredWeight) {
          appendOption(option.weight);
          break;
        }
      }
    }

    return options;
  }

} // namespace settings
