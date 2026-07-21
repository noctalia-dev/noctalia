#pragma once

#include "config/color_spec.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

using WidgetSettingStringMap = std::unordered_map<std::string, std::string>;
using WidgetSettingValue =
    std::variant<bool, std::int64_t, double, std::string, std::vector<std::string>, WidgetSettingStringMap>;

namespace noctalia::config {

  template <typename> inline constexpr bool kUnsupportedWidgetSettingType = false;

  // Converts a raw widget setting to a typed value.
  template <typename T>
  [[nodiscard]] std::optional<T> widgetSettingValueAs(const WidgetSettingValue& value, std::string_view context = {}) {
    if constexpr (std::is_same_v<T, bool>) {
      if (const auto* concrete = std::get_if<bool>(&value)) {
        return *concrete;
      }
    } else if constexpr (std::is_integral_v<T>) {
      if (const auto* concrete = std::get_if<std::int64_t>(&value)) {
        if (std::in_range<T>(*concrete)) {
          return static_cast<T>(*concrete);
        }
      }
      if (const auto* concrete = std::get_if<double>(&value)) {
        if (!std::isfinite(*concrete)) {
          return std::nullopt;
        }
        const long double rounded = std::round(static_cast<long double>(*concrete));
        if (rounded >= static_cast<long double>(std::numeric_limits<T>::lowest())
            && rounded <= static_cast<long double>(std::numeric_limits<T>::max())) {
          return static_cast<T>(rounded);
        }
      }
    } else if constexpr (std::is_floating_point_v<T>) {
      if (const auto* concrete = std::get_if<double>(&value)) {
        if (!std::isfinite(*concrete)
            || static_cast<long double>(*concrete) < static_cast<long double>(std::numeric_limits<T>::lowest())
            || static_cast<long double>(*concrete) > static_cast<long double>(std::numeric_limits<T>::max())) {
          return std::nullopt;
        }
        return static_cast<T>(*concrete);
      }
      if (const auto* concrete = std::get_if<std::int64_t>(&value)) {
        return static_cast<T>(*concrete);
      }
    } else if constexpr (std::is_same_v<T, std::string>) {
      if (const auto* concrete = std::get_if<std::string>(&value)) {
        return *concrete;
      }
    } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
      if (const auto* concrete = std::get_if<std::vector<std::string>>(&value)) {
        return *concrete;
      }
      if (const auto* concrete = std::get_if<std::string>(&value)) {
        return std::vector<std::string>{*concrete};
      }
    } else if constexpr (std::is_same_v<T, WidgetSettingStringMap>) {
      if (const auto* concrete = std::get_if<WidgetSettingStringMap>(&value)) {
        return *concrete;
      }
    } else if constexpr (std::is_same_v<T, ColorSpec>) {
      if (const auto* concrete = std::get_if<std::string>(&value)) {
        return colorSpecFromConfigString(*concrete, context);
      }
    } else {
      static_assert(kUnsupportedWidgetSettingType<T>, "unsupported widget setting type");
    }
    return std::nullopt;
  }

  // Converts a typed setting to the storage representation used by WidgetConfig.
  template <typename T> [[nodiscard]] WidgetSettingValue widgetSettingValueFrom(const T& value) {
    if constexpr (std::is_same_v<T, bool>) {
      return value;
    } else if constexpr (std::is_integral_v<T>) {
      if (!std::in_range<std::int64_t>(value)) {
        throw std::overflow_error("widget setting integer is outside the supported range");
      }
      return static_cast<std::int64_t>(value);
    } else if constexpr (std::is_floating_point_v<T>) {
      const auto converted = static_cast<double>(value);
      if (!std::isfinite(converted)) {
        throw std::overflow_error("widget setting number is not finite or is outside the supported range");
      }
      return converted;
    } else if constexpr (
        std::is_same_v<T, std::string>
        || std::is_same_v<T, std::vector<std::string>>
        || std::is_same_v<T, WidgetSettingStringMap>
    ) {
      return value;
    } else if constexpr (std::is_same_v<T, ColorSpec>) {
      return colorSpecToConfigString(value);
    } else {
      static_assert(kUnsupportedWidgetSettingType<T>, "unsupported widget setting type");
    }
  }

} // namespace noctalia::config
