#pragma once

// Internal to the config schema: field builders shared by the per-section schema sources.
// Not part of the config_schema.h API.

#include "config/schema/diagnostics.h"
#include "config/schema/field.h"
#include "config/schema/ranges.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace noctalia::config::schema {

  // Concrete ColorSpec stored as a config string; always emitted. A present
  // non-string value is a hard error (mirrors colorStringValue).
  template <typename Struct> Field<Struct> colorField(ColorSpec Struct::* member, std::string_view key) {
    return custom<Struct>(
        key,
        [member, key](const toml::table& tbl, Struct& out, std::string_view parentPath, Diagnostics&) {
          if (!tbl.contains(key)) {
            return;
          }
          auto v = tbl[key].value<std::string>();
          if (!v) {
            throw std::runtime_error(joinPath(parentPath, key) + ": expected string ColorSpec");
          }
          out.*member = colorSpecFromConfigString(*v, joinPath(parentPath, key));
        },
        [member, key](toml::table& tbl, const Struct& in) {
          tbl.insert_or_assign(key, colorSpecToConfigString(in.*member));
        }
    );
  }

  // optional<ColorSpec> stored as a config string. alwaysEmit writes
  // string-or-empty unconditionally (wallpaper.fill_color); otherwise only when
  // set (monitor overrides). A present non-string value is a hard error.
  template <typename Struct>
  Field<Struct> colorSpecField(std::optional<ColorSpec> Struct::* member, std::string_view key, bool alwaysEmit) {
    return custom<Struct>(
        key,
        [member, key](const toml::table& tbl, Struct& out, std::string_view parentPath, Diagnostics&) {
          if (!tbl.contains(key)) {
            return;
          }
          auto v = tbl[key].value<std::string>();
          if (!v) {
            throw std::runtime_error(joinPath(parentPath, key) + ": expected string ColorSpec");
          }
          if (StringUtils::trim(*v).empty()) {
            out.*member = std::nullopt;
          } else {
            out.*member = colorSpecFromConfigString(*v, joinPath(parentPath, key));
          }
        },
        [member, key, alwaysEmit](toml::table& tbl, const Struct& in) {
          if ((in.*member).has_value()) {
            tbl.insert_or_assign(key, colorSpecToConfigString(*(in.*member)));
          } else if (alwaysEmit) {
            tbl.insert_or_assign(key, std::string{});
          }
        }
    );
  }

  template <typename Struct>
  Field<Struct> optionalBoolField(std::optional<bool> Struct::* member, std::string_view key) {
    return custom<Struct>(
        key,
        [member, key](const toml::table& tbl, Struct& out, std::string_view parentPath, Diagnostics& diag) {
          if (auto v = tbl[key].value<bool>()) {
            out.*member = v;
          } else {
            warnWrongType(tbl, key, parentPath, diag, "a boolean");
          }
        },
        [member, key](toml::table& tbl, const Struct& in) {
          if ((in.*member).has_value()) {
            tbl.insert_or_assign(key, *(in.*member));
          }
        }
    );
  }

  // vector<Enum> <-> array of enum keys; pushes fallback if the parsed list is empty.
  template <typename Struct, typename Enum, std::size_t N>
  Field<Struct> enumArrayField(
      std::vector<Enum> Struct::* member, std::string_view key, const EnumOption<Enum> (&options)[N],
      std::optional<Enum> fallbackIfEmpty
  ) {
    const EnumOption<Enum>* opts = options;
    return custom<Struct>(
        key,
        [member, key, opts,
         fallbackIfEmpty](const toml::table& tbl, Struct& out, std::string_view parentPath, Diagnostics& diag) {
          if (!tbl.contains(key)) {
            return;
          }
          const auto* arr = tbl[key].as_array();
          if (arr == nullptr) {
            diag.warn(joinPath(parentPath, key), "expected an array of strings");
            return;
          }
          (out.*member).clear();
          std::size_t index = 0;
          for (const auto& item : *arr) {
            if (auto s = item.value<std::string>()) {
              const std::string trimmed = StringUtils::trim(*s);
              if (auto e = enumLookup(opts, N, trimmed)) {
                (out.*member).push_back(*e);
              } else {
                diag.warn(
                    joinPath(parentPath, key) + '[' + std::to_string(index) + ']', "unknown value \"" + *s + "\""
                );
              }
            } else {
              diag.warn(joinPath(parentPath, key) + '[' + std::to_string(index) + ']', "expected a string");
            }
            ++index;
          }
          if ((out.*member).empty() && fallbackIfEmpty) {
            (out.*member).push_back(*fallbackIfEmpty);
          }
        },
        [member, key, opts](toml::table& tbl, const Struct& in) {
          toml::array arr;
          for (auto e : in.*member) {
            const std::string_view k = enumKeyOf(opts, N, e);
            if (!k.empty()) {
              arr.push_back(std::string(k));
            }
          }
          tbl.insert_or_assign(key, std::move(arr));
        }
    );
  }

  // optional<ColorSpec>, emitted only when set, read when present. Unlike
  // colorSpecField it does NOT treat an empty string as nullopt; it matches the
  // legacy bar/capsule_group reads (which parse whatever string is present).
  template <typename Struct>
  Field<Struct> optionalColorField(std::optional<ColorSpec> Struct::* member, std::string_view key) {
    return custom<Struct>(
        key,
        [member, key](const toml::table& tbl, Struct& out, std::string_view parentPath, Diagnostics&) {
          if (!tbl.contains(key)) {
            return;
          }
          auto v = tbl[key].value<std::string>();
          if (!v) {
            throw std::runtime_error(joinPath(parentPath, key) + ": expected string ColorSpec");
          }
          out.*member = colorSpecFromConfigString(*v, joinPath(parentPath, key));
        },
        [member, key](toml::table& tbl, const Struct& in) {
          if ((in.*member).has_value()) {
            tbl.insert_or_assign(key, colorSpecToConfigString(*(in.*member)));
          }
        }
    );
  }

  // Like field(std::optional<std::string>…) but trims; a present-but-empty value stays unset so it inherits the
  // parent.
  template <typename Struct>
  Field<Struct> optionalTrimmedStringField(std::optional<std::string> Struct::* member, std::string_view key) {
    return custom<Struct>(
        key,
        [member, key](const toml::table& tbl, Struct& out, std::string_view parentPath, Diagnostics& diag) {
          if (auto v = tbl[key].value<std::string>()) {
            std::string trimmed = StringUtils::trim(*v);
            if (!trimmed.empty()) {
              out.*member = std::move(trimmed);
            }
          } else {
            warnWrongType(tbl, key, parentPath, diag, "a string");
          }
        },
        [member, key](toml::table& tbl, const Struct& in) {
          if ((in.*member).has_value()) {
            tbl.insert_or_assign(key, *(in.*member));
          }
        }
    );
  }

  template <typename Struct>
  Field<Struct>
  optionalStringVectorField(std::optional<std::vector<std::string>> Struct::* member, std::string_view key) {
    return custom<Struct>(
        key,
        [member, key](const toml::table& tbl, Struct& out, std::string_view parentPath, Diagnostics& diag) {
          if (const auto* arr = tbl[key].as_array()) {
            out.*member = readStringArray(*arr, joinPath(parentPath, key), diag);
          } else {
            warnWrongType(tbl, key, parentPath, diag, "an array of strings");
          }
        },
        [member, key](toml::table& tbl, const Struct& in) {
          if ((in.*member).has_value()) {
            toml::array arr;
            for (const auto& v : *(in.*member)) {
              arr.push_back(v);
            }
            tbl.insert_or_assign(key, std::move(arr));
          }
        }
    );
  }

  template <typename Struct>
  Field<Struct> optionalIntField(
      std::optional<std::int32_t> Struct::* member, std::string_view key,
      std::optional<Range<std::int64_t>> range = std::nullopt
  ) {
    return custom<Struct>(
        key,
        [member, key, range](const toml::table& tbl, Struct& out, std::string_view parentPath, Diagnostics& diag) {
          if (auto v = tbl[key].value<std::int64_t>()) {
            const std::int64_t value = range ? applyRange(*v, *range) : *v;
            out.*member = static_cast<std::int32_t>(value);
          } else {
            warnWrongType(tbl, key, parentPath, diag, "an integer");
          }
        },
        [member, key](toml::table& tbl, const Struct& in) {
          if ((in.*member).has_value()) {
            tbl.insert_or_assign(key, static_cast<std::int64_t>(*(in.*member)));
          }
        }
    );
  }

  template <typename Struct>
  Field<Struct> optionalFloatField(
      std::optional<float> Struct::* member, std::string_view key, std::optional<Range<float>> range = std::nullopt
  ) {
    return custom<Struct>(
        key,
        [member, key, range](const toml::table& tbl, Struct& out, std::string_view parentPath, Diagnostics& diag) {
          if (auto v = finiteDouble(tbl[key])) {
            auto value = static_cast<float>(*v);
            if (range) {
              value = applyRange(value, *range);
            }
            out.*member = value;
          } else {
            warnUnusableNumber(tbl, key, parentPath, diag);
          }
        },
        [member, key](toml::table& tbl, const Struct& in) {
          if ((in.*member).has_value()) {
            tbl.insert_or_assign(key, static_cast<double>(*(in.*member)));
          }
        }
    );
  }

  template <typename Struct>
  Field<Struct> optionalDoubleField(
      std::optional<double> Struct::* member, std::string_view key, std::optional<Range<double>> range = std::nullopt
  ) {
    return custom<Struct>(
        key,
        [member, key, range](const toml::table& tbl, Struct& out, std::string_view parentPath, Diagnostics& diag) {
          if (auto v = finiteDouble(tbl[key])) {
            out.*member = range ? applyRange(*v, *range) : *v;
          } else {
            warnUnusableNumber(tbl, key, parentPath, diag);
          }
        },
        [member, key](toml::table& tbl, const Struct& in) {
          if ((in.*member).has_value()) {
            tbl.insert_or_assign(key, *(in.*member));
          }
        }
    );
  }

} // namespace noctalia::config::schema
