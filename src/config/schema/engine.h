#pragma once

#include "config/schema/diagnostics.h"
#include "config/schema/field.h"
#include "core/toml.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace noctalia::config::schema {

  // Populate `out` from `tbl` by running every field's reader. Absent keys leave
  // the struct default. Replaces a hand-written parseConfigTable section.
  template <typename Struct>
  void readInto(
      const toml::table& tbl, Struct& out, const Schema<Struct>& schema, std::string_view parentPath, Diagnostics& diag
  ) {
    for (const auto& f : schema) {
      f.read(tbl, out, parentPath, diag);
    }
  }

  // Serialize `in` into a fresh table by running every field's writer, in schema
  // order. Replaces a hand-written config_export::serialize section.
  template <typename Struct> toml::table writeTable(const Struct& in, const Schema<Struct>& schema) {
    toml::table tbl;
    for (const auto& f : schema) {
      f.write(tbl, in);
    }
    return tbl;
  }

  // Serialize only the field named `key`; the table is empty when the schema has no such field or the
  // field does not emit (e.g. an unset optional).
  template <typename Struct>
  toml::table writeField(const Struct& in, const Schema<Struct>& schema, std::string_view key) {
    toml::table tbl;
    if (const auto it = std::ranges::find(schema, key, &Field<Struct>::key); it != schema.end()) {
      it->write(tbl, in);
    }
    return tbl;
  }

  // Append dotted paths of keys in `tbl` that no field in `schema` recognizes,
  // recursing through fields that carry a child-validator (sub-tables).
  template <typename Struct>
  void collectUnknownKeys(
      const toml::table& tbl, const Schema<Struct>& schema, std::string_view parentPath,
      std::vector<std::string>& unknown
  ) {
    std::unordered_set<std::string_view> known;
    known.reserve(schema.size());
    for (const auto& f : schema) {
      known.insert(f.key);
    }
    for (const auto& [key, node] : tbl) {
      (void)node;
      if (!known.contains(key.str())) {
        unknown.push_back(joinPath(parentPath, key.str()));
      }
    }
    for (const auto& f : schema) {
      if (f.findUnknown) {
        f.findUnknown(tbl, parentPath, unknown);
      }
    }
  }

  // Dynamic `[parent.<name>]` sub-tables read into a vector<Elem>. The map key
  // seeds each element's identity (setName); writing keys the sub-table by that
  // identity (getName) and skips empty-identity elements. The parent key is
  // emitted only when the vector is non-empty (matching the legacy guard).
  // readSkipEmptyName drops empty-identity elements on read (e.g. battery.device).
  template <typename Parent, typename Elem>
  Field<Parent> namedMap(
      std::vector<Elem> Parent::* member, std::string_view key, const Schema<Elem>& subSchema,
      std::function<void(Elem&, std::string_view name)> setName, std::function<std::string(const Elem&)> getName,
      bool readSkipEmptyName = false
  ) {
    return Field<Parent>{
        key,
        [member, key, &subSchema, setName, getName,
         readSkipEmptyName](const toml::table& tbl, Parent& out, std::string_view parentPath, Diagnostics& diag) {
          auto* map = tbl[key].as_table();
          if (map == nullptr) {
            return;
          }
          for (const auto& [name, node] : *map) {
            const auto* sub = node.as_table();
            if (sub == nullptr) {
              continue;
            }
            Elem elem{};
            setName(elem, name.str());
            readInto(*sub, elem, subSchema, joinPath(joinPath(parentPath, key), name.str()), diag);
            if (readSkipEmptyName && getName(elem).empty()) {
              continue;
            }
            (out.*member).push_back(std::move(elem));
          }
        },
        [member, key, &subSchema, getName](toml::table& tbl, const Parent& in) {
          if ((in.*member).empty()) {
            return;
          }
          toml::table map;
          for (const auto& elem : in.*member) {
            const std::string name = getName(elem);
            if (name.empty()) {
              continue;
            }
            map.insert_or_assign(name, writeTable(elem, subSchema));
          }
          tbl.insert_or_assign(key, std::move(map));
        },
        [key, &subSchema](const toml::table& tbl, std::string_view parentPath, std::vector<std::string>& unknown) {
          if (auto* map = tbl[key].as_table()) {
            for (const auto& [name, node] : *map) {
              if (auto* sub = node.as_table()) {
                collectUnknownKeys(*sub, subSchema, joinPath(joinPath(parentPath, key), name.str()), unknown);
              }
            }
          }
        },
    };
  }

  // Array-of-tables read into a vector<Elem> (e.g. control_center.shortcuts).
  // On read the vector is cleared then filled with
  // elements that pass `keep`; on write only `keep` elements are emitted. The
  // array key is always written (possibly empty), matching the legacy emitters.
  template <typename Parent, typename Elem>
  Field<Parent> arrayOf(
      std::vector<Elem> Parent::* member, std::string_view key, const Schema<Elem>& subSchema,
      std::function<bool(const Elem&)> keep
  ) {
    return Field<Parent>{
        key,
        [member, key, &subSchema,
         keep](const toml::table& tbl, Parent& out, std::string_view parentPath, Diagnostics& diag) {
          auto* arr = tbl[key].as_array();
          if (arr == nullptr) {
            return;
          }
          (out.*member).clear();
          for (const auto& node : *arr) {
            const auto* sub = node.as_table();
            if (sub == nullptr) {
              continue;
            }
            Elem elem{};
            readInto(*sub, elem, subSchema, joinPath(parentPath, key), diag);
            if (keep(elem)) {
              (out.*member).push_back(std::move(elem));
            }
          }
        },
        [member, key, &subSchema, keep](toml::table& tbl, const Parent& in) {
          toml::array arr;
          for (const auto& elem : in.*member) {
            if (!keep(elem)) {
              continue;
            }
            arr.push_back(writeTable(elem, subSchema));
          }
          tbl.insert_or_assign(key, std::move(arr));
        },
        [key, &subSchema](const toml::table& tbl, std::string_view parentPath, std::vector<std::string>& unknown) {
          if (auto* arr = tbl[key].as_array()) {
            for (const auto& node : *arr) {
              if (auto* sub = node.as_table()) {
                collectUnknownKeys(*sub, subSchema, joinPath(parentPath, key), unknown);
              }
            }
          }
        },
    };
  }

  // Nested struct under a fixed key (e.g. shell.shadow). Reads/writes via the
  // sub-schema and recurses for unknown-key detection.
  template <typename Struct, typename Sub>
  Field<Struct> subTable(Sub Struct::* member, std::string_view key, const Schema<Sub>& subSchema) {
    return Field<Struct>{
        key,
        [member, key, &subSchema](const toml::table& tbl, Struct& out, std::string_view parentPath, Diagnostics& diag) {
          if (auto* sub = tbl[key].as_table()) {
            readInto(*sub, out.*member, subSchema, joinPath(parentPath, key), diag);
          }
        },
        [member, key, &subSchema](toml::table& tbl, const Struct& in) {
          tbl.insert_or_assign(key, writeTable(in.*member, subSchema));
        },
        [key, &subSchema](const toml::table& tbl, std::string_view parentPath, std::vector<std::string>& unknown) {
          if (auto* sub = tbl[key].as_table()) {
            collectUnknownKeys(*sub, subSchema, joinPath(parentPath, key), unknown);
          }
        },
    };
  }

} // namespace noctalia::config::schema
