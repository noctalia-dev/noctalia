#pragma once

#include "config/config_types.h"
#include "config/schema/diagnostics.h"
#include "core/toml.h"
#include "util/string_utils.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noctalia::config::schema {

  // Optional numeric constraint carried by a Field. Single source for parse-time
  // clamping, GUI slider bounds, and range validation. Either bound may be
  // omitted (e.g. a min-only floor); `step` is GUI metadata only.
  template <typename T> struct Range {
    std::optional<T> min = std::nullopt;
    std::optional<T> max = std::nullopt;
    std::optional<T> step = std::nullopt;
  };

  template <typename T> T applyRange(T value, const Range<T>& range) {
    if (range.min && value < *range.min) {
      value = *range.min;
    }
    if (range.max && value > *range.max) {
      value = *range.max;
    }
    return value;
  }

  // Mirror of ConfigService's finiteDouble: accept a double or an int, reject
  // non-finite. Keeps float/double reads behaviorally identical to the old code.
  inline std::optional<double> finiteDouble(const toml::node_view<const toml::node>& node) {
    if (auto v = node.value<double>()) {
      if (!std::isfinite(*v)) {
        return std::nullopt;
      }
      return v;
    }
    if (auto v = node.value<std::int64_t>()) {
      return static_cast<double>(*v);
    }
    return std::nullopt;
  }

  // One descriptor: binds a single TOML key in a parent table to part of a
  // Struct. read() pulls the key into `out`; write() emits it from `in`. Both
  // are no-ops when the key is absent (read) — leaving the struct default.
  template <typename Struct> struct Field {
    std::string_view key;
    std::function<void(const toml::table& tbl, Struct& out, std::string_view parentPath, Diagnostics& diag)> read;
    std::function<void(toml::table& tbl, const Struct& in)> write;
    // Set only for composite fields (sub-tables/named-maps/arrays): recurse into
    // this key and append dotted paths of unrecognized child keys. Null for leaves.
    std::function<void(const toml::table& tbl, std::string_view parentPath, std::vector<std::string>& unknown)>
        findUnknown = nullptr;
  };

  // Ordered set of fields for one struct. Insertion order is the serialization
  // order — keep it identical to the config_export::serialize emission order.
  template <typename Struct> using Schema = std::vector<Field<Struct>>;

  // ── Leaf codec factories ─────────────────────────────────────────────────

  template <typename Struct> Field<Struct> field(bool Struct::* member, std::string_view key) {
    return Field<Struct>{
        key,
        [member, key](const toml::table& tbl, Struct& out, std::string_view, Diagnostics&) {
          if (auto v = tbl[key].value<bool>()) {
            out.*member = *v;
          }
        },
        [member, key](toml::table& tbl, const Struct& in) { tbl.insert_or_assign(key, in.*member); },
    };
  }

  template <typename Struct> Field<Struct> field(std::string Struct::* member, std::string_view key) {
    return Field<Struct>{
        key,
        [member, key](const toml::table& tbl, Struct& out, std::string_view, Diagnostics&) {
          if (auto v = tbl[key].value<std::string>()) {
            out.*member = *v;
          }
        },
        [member, key](toml::table& tbl, const Struct& in) { tbl.insert_or_assign(key, in.*member); },
    };
  }

  template <typename Struct>
  Field<Struct>
  field(std::int32_t Struct::* member, std::string_view key, std::optional<Range<std::int64_t>> range = std::nullopt) {
    return Field<Struct>{
        key,
        [member, key, range](const toml::table& tbl, Struct& out, std::string_view, Diagnostics&) {
          if (auto v = tbl[key].value<std::int64_t>()) {
            std::int64_t value = *v;
            if (range) {
              value = applyRange(value, *range);
            }
            out.*member = static_cast<std::int32_t>(value);
          }
        },
        [member, key](toml::table& tbl, const Struct& in) {
          tbl.insert_or_assign(key, static_cast<std::int64_t>(in.*member));
        },
    };
  }

  template <typename Struct>
  Field<Struct> field(float Struct::* member, std::string_view key, std::optional<Range<float>> range = std::nullopt) {
    return Field<Struct>{
        key,
        [member, key, range](const toml::table& tbl, Struct& out, std::string_view, Diagnostics&) {
          if (auto v = finiteDouble(tbl[key])) {
            auto value = static_cast<float>(*v);
            if (range) {
              value = applyRange(value, *range);
            }
            out.*member = value;
          }
        },
        [member, key](toml::table& tbl, const Struct& in) {
          tbl.insert_or_assign(key, static_cast<double>(in.*member));
        },
    };
  }

  template <typename Struct>
  Field<Struct>
  field(double Struct::* member, std::string_view key, std::optional<Range<double>> range = std::nullopt) {
    return Field<Struct>{
        key,
        [member, key, range](const toml::table& tbl, Struct& out, std::string_view, Diagnostics&) {
          if (auto v = finiteDouble(tbl[key])) {
            double value = *v;
            if (range) {
              value = applyRange(value, *range);
            }
            out.*member = value;
          }
        },
        [member, key](toml::table& tbl, const Struct& in) { tbl.insert_or_assign(key, in.*member); },
    };
  }

  // Optional scalar written only when set (mirrors `if (x.has_value()) emit`).
  // Read accepts an int or finite double, matching finiteDouble.
  template <typename Struct> Field<Struct> field(std::optional<double> Struct::* member, std::string_view key) {
    return Field<Struct>{
        key,
        [member, key](const toml::table& tbl, Struct& out, std::string_view, Diagnostics&) {
          if (auto v = finiteDouble(tbl[key])) {
            out.*member = v;
          }
        },
        [member, key](toml::table& tbl, const Struct& in) {
          if ((in.*member).has_value()) {
            tbl.insert_or_assign(key, *(in.*member));
          }
        },
    };
  }

  template <typename Struct> Field<Struct> field(std::vector<std::string> Struct::* member, std::string_view key) {
    return Field<Struct>{
        key,
        [member, key](const toml::table& tbl, Struct& out, std::string_view, Diagnostics&) {
          if (auto* arr = tbl[key].as_array()) {
            std::vector<std::string> values;
            for (const auto& item : *arr) {
              if (auto s = item.value<std::string>()) {
                values.push_back(*s);
              }
            }
            out.*member = std::move(values);
          }
        },
        [member, key](toml::table& tbl, const Struct& in) {
          toml::array arr;
          for (const auto& value : in.*member) {
            arr.push_back(value);
          }
          tbl.insert_or_assign(key, std::move(arr));
        },
    };
  }

  // Escape hatch for a single key whose read/write don't fit a stock codec
  // (e.g. a value that cascades to sibling fields, or bespoke validation).
  template <typename Struct>
  Field<Struct> custom(
      std::string_view key,
      std::function<void(const toml::table& tbl, Struct& out, std::string_view parentPath, Diagnostics& diag)> read,
      std::function<void(toml::table& tbl, const Struct& in)> write
  ) {
    return Field<Struct>{key, std::move(read), std::move(write)};
  }

  // A keyless field that runs cross-field logic after all leaf reads (enforcing
  // invariants a per-field codec can't express, e.g. day > night). Writes
  // nothing. Place it last in a Schema so it sees the fully-read struct.
  template <typename Struct>
  Field<Struct> finalize(std::function<void(Struct& out, std::string_view parentPath, Diagnostics& diag)> fn) {
    return Field<Struct>{
        "",
        [fn = std::move(fn)](const toml::table&, Struct& out, std::string_view parentPath, Diagnostics& diag) {
          fn(out, parentPath, diag);
        },
        [](toml::table&, const Struct&) {},
    };
  }

  // Pointer+count views over an EnumOption<E>[] table, so codecs can capture a
  // stable pointer (the tables are static constexpr) instead of an array ref.
  template <typename Enum>
  std::optional<Enum> enumLookup(const EnumOption<Enum>* opts, std::size_t n, std::string_view key) {
    for (std::size_t i = 0; i < n; ++i) {
      if (opts[i].key == key) {
        return opts[i].value;
      }
    }
    return std::nullopt;
  }

  template <typename Enum> std::string_view enumKeyOf(const EnumOption<Enum>* opts, std::size_t n, Enum value) {
    for (std::size_t i = 0; i < n; ++i) {
      if (opts[i].value == value) {
        return opts[i].key;
      }
    }
    return {};
  }

  // Optional enum: read like enumField, but written only when the optional is
  // set (mirrors `if (x.has_value()) emit`).
  template <typename Struct, typename Enum, std::size_t N>
  Field<Struct>
  optionalEnumField(std::optional<Enum> Struct::* member, std::string_view key, const EnumOption<Enum> (&options)[N]) {
    const EnumOption<Enum>* opts = options;
    return Field<Struct>{
        key,
        [member, key, opts](const toml::table& tbl, Struct& out, std::string_view parentPath, Diagnostics& diag) {
          if (auto v = tbl[key].value<std::string>()) {
            const std::string trimmed = StringUtils::trim(*v);
            if (auto parsed = enumLookup(opts, N, trimmed)) {
              out.*member = parsed;
            } else {
              diag.warn(joinPath(parentPath, key), "unknown value \"" + *v + "\"");
            }
          }
        },
        [member, key, opts](toml::table& tbl, const Struct& in) {
          if ((in.*member).has_value()) {
            tbl.insert_or_assign(key, std::string(enumKeyOf(opts, N, *(in.*member))));
          }
        },
    };
  }

  // Enum field backed by an existing EnumOption<E>[] table. Trims input, and
  // warns (does not fail) on unknown keys — matching the legacy behavior.
  template <typename Struct, typename Enum, std::size_t N>
  Field<Struct> enumField(Enum Struct::* member, std::string_view key, const EnumOption<Enum> (&options)[N]) {
    const EnumOption<Enum>* opts = options;
    return Field<Struct>{
        key,
        [member, key, opts](const toml::table& tbl, Struct& out, std::string_view parentPath, Diagnostics& diag) {
          if (auto v = tbl[key].value<std::string>()) {
            const std::string trimmed = StringUtils::trim(*v);
            if (auto parsed = enumLookup(opts, N, trimmed)) {
              out.*member = *parsed;
            } else {
              diag.warn(joinPath(parentPath, key), "unknown value \"" + *v + "\"");
            }
          }
        },
        [member, key, opts](toml::table& tbl, const Struct& in) {
          tbl.insert_or_assign(key, std::string(enumKeyOf(opts, N, in.*member)));
        },
    };
  }

} // namespace noctalia::config::schema
