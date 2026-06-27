#pragma once

#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace ui {

  // A single prop value in a declarative UI tree. Numbers are always double
  // (Luau numbers); number arrays carry data series (e.g. graph values) and
  // string arrays carry option lists (e.g. select options).
  using UiTreeValue = std::variant<bool, double, std::string, std::vector<double>, std::vector<std::string>>;

  // One node of a declarative control tree produced by plugin code. Pure data:
  // it crosses the script-worker → UI-thread boundary by value and is mapped to
  // retained `src/ui/controls/` instances by `UiTreeReconciler`.
  struct UiTreeNode {
    std::string type; // "column", "row", "label", "glyph", ...
    std::string key;  // stable identity for keyed reconciliation ("" = positional)
    std::unordered_map<std::string, UiTreeValue> props;
    std::vector<UiTreeNode> children;

    bool operator==(const UiTreeNode&) const = default;
  };

} // namespace ui
