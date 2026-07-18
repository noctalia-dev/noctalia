#pragma once

#include "config/config_types.h"
#include "config/schema/widget_schema.h"
#include "shell/settings/widget_settings_registry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace noctalia::bar {

  using WidgetSettingChoiceValue = std::variant<std::int64_t, std::string>;

  template <typename T> struct WidgetSettingChoice {
    T value;
    WidgetSettingChoiceValue configValue;
    std::string labelKey;
  };

  template <typename T> struct WidgetFieldSpec {
    std::string_view key;
    std::optional<settings::WidgetControlKind> control;
    std::optional<double> minValue;
    std::optional<double> maxValue;
    std::optional<double> step;
    std::vector<WidgetSettingChoice<T>> choices;
    std::optional<settings::WidgetSettingPresentation> presentation = settings::WidgetSettingPresentation{};
  };

  template <typename Options> struct WidgetDefinitionField {
    config::schema::WidgetSettingField schema;
    std::optional<settings::WidgetSettingPresentation> presentation;
    std::function<void(Options&, const WidgetConfig*, std::string_view)> resolve;
  };

  namespace detail {

    template <typename> struct MemberPointerTraits;

    template <typename Options, typename T> struct MemberPointerTraits<T Options::*> {
      using OptionsType = Options;
      using ValueType = T;
    };

    template <typename T>
    inline constexpr bool kDirectWidgetSettingValue = std::is_same_v<T, bool>
        || std::is_integral_v<T>
        || std::is_floating_point_v<T>
        || std::is_same_v<T, std::string>
        || std::is_same_v<T, std::vector<std::string>>
        || std::is_same_v<T, ColorSpec>;

    template <typename T>
    config::schema::WidgetSettingType defaultSchemaType(const std::vector<WidgetSettingChoice<T>>& choices) {
      if (!choices.empty() || std::is_enum_v<T>) {
        return config::schema::WidgetSettingType::Enum;
      }
      if constexpr (std::is_same_v<T, bool>) {
        return config::schema::WidgetSettingType::Bool;
      } else if constexpr (std::is_integral_v<T>) {
        return config::schema::WidgetSettingType::Int;
      } else if constexpr (std::is_floating_point_v<T>) {
        return config::schema::WidgetSettingType::Double;
      } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
        return config::schema::WidgetSettingType::StringList;
      } else if constexpr (std::is_same_v<T, ColorSpec>) {
        return config::schema::WidgetSettingType::Color;
      } else {
        return config::schema::WidgetSettingType::String;
      }
    }

    template <typename T>
    settings::WidgetControlKind defaultControl(
        const std::vector<WidgetSettingChoice<T>>& choices,
        const std::optional<settings::WidgetSettingPresentation>& presentation
    ) {
      if (!choices.empty()
          || std::is_enum_v<T>
          || (presentation.has_value()
              && (presentation->optionSource != settings::WidgetSettingOptionSource::Static
                  || !presentation->options.empty()))) {
        return settings::WidgetControlKind::Select;
      }
      if constexpr (std::is_same_v<T, bool>) {
        return settings::WidgetControlKind::Bool;
      } else if constexpr (std::is_integral_v<T>) {
        return settings::WidgetControlKind::Int;
      } else if constexpr (std::is_floating_point_v<T>) {
        return settings::WidgetControlKind::Double;
      } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
        return settings::WidgetControlKind::StringList;
      } else if constexpr (std::is_same_v<T, ColorSpec>) {
        return settings::WidgetControlKind::ColorSpec;
      } else {
        return settings::WidgetControlKind::String;
      }
    }

    inline std::string choiceConfigText(const WidgetSettingChoiceValue& value) {
      return std::visit(
          [](const auto& concrete) -> std::string {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, std::string>) {
              return concrete;
            } else {
              return std::to_string(concrete);
            }
          },
          value
      );
    }

    inline WidgetSettingValue choiceSettingValue(const WidgetSettingChoiceValue& value) {
      return std::visit([](const auto& concrete) -> WidgetSettingValue { return concrete; }, value);
    }

    template <typename T>
    void validateChoices(const std::vector<WidgetSettingChoice<T>>& choices, std::string_view key) {
      for (std::size_t i = 0; i < choices.size(); ++i) {
        for (std::size_t j = i + 1; j < choices.size(); ++j) {
          if (choiceConfigText(choices[i].configValue) == choiceConfigText(choices[j].configValue)) {
            throw std::logic_error(
                std::format(
                    "widget field '{}' has duplicate choice value '{}'", key, choiceConfigText(choices[i].configValue)
                )
            );
          }
          if (choices[i].value == choices[j].value) {
            throw std::logic_error(std::format("widget field '{}' has duplicate typed choices", key));
          }
          if (choices[i].configValue.index() != choices[j].configValue.index()) {
            throw std::logic_error(std::format("widget field '{}' mixes string and integer choice values", key));
          }
        }
      }
    }

    template <typename T>
    WidgetSettingValue
    settingValueFor(const T& value, const std::vector<WidgetSettingChoice<T>>& choices, std::string_view key) {
      if (!choices.empty()) {
        const auto choice = std::ranges::find(choices, value, &WidgetSettingChoice<T>::value);
        if (choice != choices.end()) {
          return choiceSettingValue(choice->configValue);
        }
        throw std::logic_error(
            std::format("widget field '{}' has a default value that is not represented by its choices", key)
        );
      }
      if constexpr (kDirectWidgetSettingValue<T>) {
        return config::widgetSettingValueFrom(value);
      } else {
        throw std::logic_error(std::format("widget field '{}' requires choices", key));
      }
    }

    template <typename T> void validateStep(std::optional<double> step, std::string_view key) {
      if (!step.has_value()) {
        return;
      }
      if constexpr (std::is_same_v<T, bool> || (!std::is_integral_v<T> && !std::is_floating_point_v<T>)) {
        throw std::logic_error(std::format("non-numeric widget field '{}' declares a numeric step", key));
      } else if (!std::isfinite(*step) || *step <= 0.0) {
        throw std::logic_error(std::format("widget field '{}' has an invalid numeric step", key));
      } else if constexpr (std::is_integral_v<T>) {
        if (std::trunc(*step) != *step) {
          throw std::logic_error(std::format("integral widget field '{}' declares a fractional step", key));
        }
      }
    }

    template <typename T>
    using NumericRangeValue =
        std::conditional_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, std::int64_t, double>;

    template <typename T> struct EffectiveNumericRange {
      NumericRangeValue<T> lower{};
      NumericRangeValue<T> upper{};
      std::optional<double> schemaMin;
      std::optional<double> schemaMax;
    };

    template <typename T>
    EffectiveNumericRange<T>
    effectiveNumericRange(std::optional<double> minValue, std::optional<double> maxValue, std::string_view key) {
      if constexpr (std::is_same_v<T, bool> || (!std::is_integral_v<T> && !std::is_floating_point_v<T>)) {
        if (minValue.has_value() || maxValue.has_value()) {
          throw std::logic_error(std::format("non-numeric widget field '{}' declares a numeric range", key));
        }
        return {};
      } else if constexpr (std::is_integral_v<T>) {
        auto lower = std::numeric_limits<std::int64_t>::lowest();
        auto upper = std::numeric_limits<std::int64_t>::max();
        constexpr auto storageDigits = std::numeric_limits<std::int64_t>::digits;
        if constexpr (std::is_signed_v<T>) {
          if constexpr (std::numeric_limits<T>::digits < storageDigits) {
            lower = static_cast<std::int64_t>(std::numeric_limits<T>::lowest());
            upper = static_cast<std::int64_t>(std::numeric_limits<T>::max());
          }
        } else {
          lower = 0;
          if constexpr (std::numeric_limits<T>::digits < storageDigits) {
            upper = static_cast<std::int64_t>(std::numeric_limits<T>::max());
          }
        }

        constexpr double int64Lower = -0x1p63;
        constexpr double int64UpperExclusive = 0x1p63;
        if (minValue.has_value()) {
          if (!std::isfinite(*minValue)) {
            throw std::logic_error(std::format("widget field '{}' has a non-finite minimum", key));
          }
          const double concrete = std::ceil(*minValue);
          if (concrete >= int64UpperExclusive) {
            throw std::logic_error(std::format("widget field '{}' has an empty numeric range", key));
          }
          if (concrete > int64Lower) {
            lower = std::max(lower, static_cast<std::int64_t>(concrete));
          }
        }
        if (maxValue.has_value()) {
          if (!std::isfinite(*maxValue)) {
            throw std::logic_error(std::format("widget field '{}' has a non-finite maximum", key));
          }
          const double concrete = std::floor(*maxValue);
          if (concrete < int64Lower) {
            throw std::logic_error(std::format("widget field '{}' has an empty numeric range", key));
          }
          if (concrete < int64UpperExclusive) {
            upper = std::min(upper, static_cast<std::int64_t>(concrete));
          }
        }
        if (lower > upper) {
          throw std::logic_error(std::format("widget field '{}' has an empty numeric range", key));
        }

        return EffectiveNumericRange<T>{
            .lower = lower,
            .upper = upper,
            .schemaMin = lower > std::numeric_limits<std::int64_t>::lowest()
                ? std::optional<double>(static_cast<double>(lower))
                : std::nullopt,
            .schemaMax = upper < std::numeric_limits<std::int64_t>::max()
                ? std::optional<double>(static_cast<double>(upper))
                : std::nullopt,
        };
      } else {
        const auto convertedLower = static_cast<double>(std::numeric_limits<T>::lowest());
        const auto convertedUpper = static_cast<double>(std::numeric_limits<T>::max());
        auto lower = std::isfinite(convertedLower) ? convertedLower : std::numeric_limits<double>::lowest();
        auto upper = std::isfinite(convertedUpper) ? convertedUpper : std::numeric_limits<double>::max();
        if (minValue.has_value()) {
          if (!std::isfinite(*minValue)) {
            throw std::logic_error(std::format("widget field '{}' has a non-finite minimum", key));
          }
          lower = std::max(lower, *minValue);
        }
        if (maxValue.has_value()) {
          if (!std::isfinite(*maxValue)) {
            throw std::logic_error(std::format("widget field '{}' has a non-finite maximum", key));
          }
          upper = std::min(upper, *maxValue);
        }
        if (lower > upper) {
          throw std::logic_error(std::format("widget field '{}' has an empty numeric range", key));
        }

        return EffectiveNumericRange<T>{
            .lower = lower,
            .upper = upper,
            .schemaMin = lower > std::numeric_limits<double>::lowest() ? std::optional<double>(lower) : std::nullopt,
            .schemaMax = upper < std::numeric_limits<double>::max() ? std::optional<double>(upper) : std::nullopt,
        };
      }
    }

    template <typename T> T validateDefaultRange(T value, const EffectiveNumericRange<T>& range, std::string_view key) {
      if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
        if (!std::in_range<std::int64_t>(value)) {
          throw std::logic_error(std::format("widget field '{}' has a default value outside its storage range", key));
        }
        const auto concrete = static_cast<std::int64_t>(value);
        if (concrete < range.lower || concrete > range.upper) {
          throw std::logic_error(std::format("widget field '{}' has a default value outside its range", key));
        }
      } else if constexpr (std::is_floating_point_v<T>) {
        const auto concrete = static_cast<double>(value);
        if (!std::isfinite(concrete)) {
          throw std::logic_error(std::format("widget field '{}' has a non-finite default value", key));
        }
        if (concrete < range.lower || concrete > range.upper) {
          throw std::logic_error(std::format("widget field '{}' has a default value outside its range", key));
        }
      }
      return value;
    }

    template <typename T>
    std::optional<T> numericSettingValueAs(const WidgetSettingValue& value, const EffectiveNumericRange<T>& range) {
      if constexpr (std::is_integral_v<T>) {
        std::int64_t concrete = 0;
        if (const auto* integerValue = std::get_if<std::int64_t>(&value)) {
          concrete = *integerValue;
        } else if (const auto* doubleValue = std::get_if<double>(&value)) {
          if (!std::isfinite(*doubleValue)) {
            return std::nullopt;
          }
          const double rounded = std::round(*doubleValue);
          constexpr double int64Lower = -0x1p63;
          constexpr double int64UpperExclusive = 0x1p63;
          if (rounded <= int64Lower) {
            concrete = std::numeric_limits<std::int64_t>::lowest();
          } else if (rounded >= int64UpperExclusive) {
            concrete = std::numeric_limits<std::int64_t>::max();
          } else {
            concrete = static_cast<std::int64_t>(rounded);
          }
        } else {
          return std::nullopt;
        }
        return static_cast<T>(std::clamp(concrete, range.lower, range.upper));
      } else {
        double concrete = 0.0;
        if (const auto* integerValue = std::get_if<std::int64_t>(&value)) {
          concrete = static_cast<double>(*integerValue);
        } else if (const auto* doubleValue = std::get_if<double>(&value)) {
          if (!std::isfinite(*doubleValue)) {
            return std::nullopt;
          }
          concrete = *doubleValue;
        } else {
          return std::nullopt;
        }
        return static_cast<T>(std::clamp(concrete, range.lower, range.upper));
      }
    }

    template <typename T>
    std::optional<T> settingValueAs(
        const WidgetSettingValue& value, const std::vector<WidgetSettingChoice<T>>& choices,
        const EffectiveNumericRange<T>& range, std::string_view context
    ) {
      if (!choices.empty()) {
        std::string configured;
        if (const auto* stringValue = std::get_if<std::string>(&value)) {
          configured = *stringValue;
        } else if (const auto* integerValue = std::get_if<std::int64_t>(&value)) {
          configured = std::to_string(*integerValue);
        } else {
          return std::nullopt;
        }
        const auto choice = std::ranges::find_if(choices, [&](const WidgetSettingChoice<T>& candidate) {
          return choiceConfigText(candidate.configValue) == configured;
        });
        if (choice != choices.end()) {
          return choice->value;
        }
        return std::nullopt;
      }
      if constexpr ((std::is_integral_v<T> && !std::is_same_v<T, bool>) || std::is_floating_point_v<T>) {
        return numericSettingValueAs<T>(value, range);
      } else if constexpr (kDirectWidgetSettingValue<T>) {
        return config::widgetSettingValueAs<T>(value, context);
      } else {
        return std::nullopt;
      }
    }

    inline std::string settingTranslationSegment(std::string_view key) {
      std::string segment(key);
      std::ranges::replace(segment, '_', '-');
      return segment;
    }

  } // namespace detail

  template <auto Member>
  auto field(WidgetFieldSpec<typename detail::MemberPointerTraits<decltype(Member)>::ValueType> spec) {
    using Traits = detail::MemberPointerTraits<decltype(Member)>;
    using Options = typename Traits::OptionsType;
    using T = typename Traits::ValueType;

    if (spec.key.empty()) {
      throw std::logic_error("widget field key cannot be empty");
    }
    const std::string key(spec.key);
    detail::validateChoices(spec.choices, spec.key);
    if (!spec.choices.empty() && (spec.minValue.has_value() || spec.maxValue.has_value() || spec.step.has_value())) {
      throw std::logic_error(std::format("choice-backed widget field '{}' declares numeric range metadata", spec.key));
    }
    detail::validateStep<T>(spec.step, spec.key);
    const auto effectiveRange = spec.choices.empty()
        ? detail::effectiveNumericRange<T>(spec.minValue, spec.maxValue, spec.key)
        : detail::EffectiveNumericRange<T>{};
    const Options defaults{};
    const T defaultValue = spec.choices.empty()
        ? detail::validateDefaultRange(defaults.*Member, effectiveRange, spec.key)
        : defaults.*Member;
    config::schema::WidgetSettingField schema{
        .key = key,
        .type = detail::defaultSchemaType(spec.choices),
        .defaultValue = detail::settingValueFor(defaultValue, spec.choices, spec.key),
        .minValue = effectiveRange.schemaMin,
        .maxValue = effectiveRange.schemaMax,
        .step = spec.step,
    };
    schema.enumValues.reserve(spec.choices.size());
    std::ranges::transform(
        spec.choices, std::back_inserter(schema.enumValues),
        [](const WidgetSettingChoice<T>& choice) { return detail::choiceConfigText(choice.configValue); }
    );

    if (spec.presentation.has_value()) {
      spec.presentation->control = spec.control.value_or(detail::defaultControl(spec.choices, spec.presentation));
      if (!spec.choices.empty() && spec.choices.front().configValue.index() == 0) {
        spec.presentation->integerValue = true;
      }
      if (spec.presentation->options.empty()) {
        spec.presentation->options.reserve(spec.choices.size());
        std::ranges::transform(
            spec.choices, std::back_inserter(spec.presentation->options), [](const WidgetSettingChoice<T>& choice) {
              return settings::WidgetSettingSelectOption{
                  .value = detail::choiceConfigText(choice.configValue),
                  .labelKey = choice.labelKey,
              };
            }
        );
      }
      const std::string translationSegment = detail::settingTranslationSegment(spec.key);
      if (spec.presentation->labelKey.empty()) {
        spec.presentation->labelKey = std::format("settings.widgets.settings.{}.label", translationSegment);
      }
      if (spec.presentation->descriptionKey.empty()) {
        spec.presentation->descriptionKey = std::format("settings.widgets.settings.{}.description", translationSegment);
      }
    }

    return WidgetDefinitionField<Options>{
        .schema = std::move(schema),
        .presentation = std::move(spec.presentation),
        .resolve = [key, defaultValue, choices = std::move(spec.choices),
                    effectiveRange](Options& options, const WidgetConfig* config, std::string_view context) {
          T value = defaultValue;
          if (config != nullptr) {
            if (const auto* configured = config->findSetting(key)) {
              const std::string fieldContext = context.empty() ? key : std::format("{}.{}", context, key);
              if (auto decoded = detail::settingValueAs<T>(*configured, choices, effectiveRange, fieldContext)) {
                value = std::move(*decoded);
              }
            }
          }
          options.*Member = std::move(value);
        },
    };
  }

  template <typename Options, typename Context = std::monostate> struct WidgetDefinition {
    std::string_view type;
    std::vector<WidgetDefinitionField<Options>> fields;
    std::function<void(Options&, const Context&)> finalize;

    [[nodiscard]] Options resolve(const WidgetConfig* config, std::string_view settingContext) const
      requires std::is_same_v<Context, std::monostate>
    {
      return resolve(config, settingContext, std::monostate{});
    }

    [[nodiscard]] Options
    resolve(const WidgetConfig* config, std::string_view settingContext, const Context& context) const {
      validate();
      // WidgetConfig comes from ConfigService's schema-validated configuration.
      // Resolution still falls back to the semantic Options default if a value
      // cannot be decoded, keeping programmatic callers safe as well.
      Options options{};
      for (const auto& definitionField : fields) {
        definitionField.resolve(options, config, settingContext);
      }
      if (finalize) {
        finalize(options, context);
      }
      return options;
    }

    [[nodiscard]] config::schema::WidgetSettingSchema schemaFields() const {
      validate();
      config::schema::WidgetSettingSchema schema;
      schema.reserve(fields.size());
      std::ranges::transform(
          fields, std::back_inserter(schema),
          [](const WidgetDefinitionField<Options>& definitionField) { return definitionField.schema; }
      );
      return schema;
    }

    [[nodiscard]] std::vector<settings::WidgetSettingSpec> presentedSettingSpecs() const {
      validate();
      return fields
          | std::views::filter([](const WidgetDefinitionField<Options>& definitionField) {
               return definitionField.presentation.has_value();
             })
          | std::views::transform([](const WidgetDefinitionField<Options>& definitionField) {
               settings::WidgetSettingSpec spec;
               static_cast<settings::WidgetSettingPresentation&>(spec) = *definitionField.presentation;
               spec.schema = definitionField.schema;
               return spec;
             })
          | std::ranges::to<std::vector>();
    }

  private:
    void validate() const {
      if (type.empty()) {
        throw std::logic_error("widget definition type cannot be empty");
      }
      for (std::size_t i = 0; i < fields.size(); ++i) {
        for (std::size_t j = i + 1; j < fields.size(); ++j) {
          if (fields[i].schema.key == fields[j].schema.key) {
            throw std::logic_error(
                std::format("widget definition '{}' has duplicate field '{}'", type, fields[i].schema.key)
            );
          }
        }
      }
    }
  };

} // namespace noctalia::bar
