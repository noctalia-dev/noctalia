#pragma once

#include "i18n/i18n_service.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// Free-function translation API. Call sites look like:
//
//   i18n::tr("clipboard.title")
//   i18n::tr("settings.entities.bar.label", "name", "top")
//
// Trailing arguments are alternating name/value pairs. Values may be any type
// formattable by std::format; names must be convertible to string_view.
// A missing key renders as "!!key!!" so untranslated strings are visible.

namespace i18n {

  namespace detail {

    template <typename T> inline std::string argToString(T&& v) {
      using U = std::remove_cvref_t<T>;
      if constexpr (std::is_convertible_v<const U&, std::string_view>) {
        return std::string(std::string_view(v));
      } else {
        return std::format("{}", std::forward<T>(v));
      }
    }

    using Pair = std::pair<std::string_view, std::string>;

    std::string interpolate(std::string_view tmpl, std::span<const Pair> args);

    template <typename Name, typename Value, typename... Rest>
    void fillPairs(Pair* pairs, std::size_t idx, Name&& name, Value&& value, Rest&&... rest) {
      pairs[idx] = Pair{std::string_view(name), argToString(std::forward<Value>(value))};
      if constexpr (sizeof...(Rest) > 0) {
        fillPairs(pairs, idx + 1, std::forward<Rest>(rest)...);
      }
    }

  } // namespace detail

  // Convert a snake_case config/wire identifier into the dash-case segment used
  // in translation keys (e.g. "art_size" -> "art-size").
  inline std::string keySegment(std::string_view id) {
    std::string out(id);
    std::ranges::replace(out, '_', '-');
    return out;
  }

  template <typename... Args> std::string tr(std::string_view key, Args&&... args) {
    static_assert(sizeof...(Args) % 2 == 0, "i18n::tr() requires an even number of trailing args (name, value pairs)");
    auto raw = Service::instance().lookup(key);
    if (raw.empty()) {
      return std::format("!!{}!!", key);
    }
    if constexpr (sizeof...(Args) == 0) {
      return std::string(raw);
    } else {
      constexpr std::size_t kPairs = sizeof...(Args) / 2;
      std::array<detail::Pair, kPairs> pairs;
      detail::fillPairs(pairs.data(), 0, std::forward<Args>(args)...);
      return detail::interpolate(raw, std::span<const detail::Pair>(pairs.data(), pairs.size()));
    }
  }

  template <typename... Args> std::string trp(std::string_view key, long count, Args&&... args) {
    if (count == 1) {
      return tr(key, "count", count, std::forward<Args>(args)...);
    }
    std::string realKey(key);
    realKey += "-plural";
    return tr(std::string_view(realKey), "count", count, std::forward<Args>(args)...);
  }

} // namespace i18n
