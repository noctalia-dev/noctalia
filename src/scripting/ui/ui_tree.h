#pragma once

#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace ui {

  // One ui.segmented option: the label shown in the segment, the optional glyph
  // drawn beside it, the optional tooltip (required for a glyph-only option),
  // and the value the onChange callback reports (a bare string option defaults
  // to label = value, no glyph).
  struct UiOption {
    std::string label;
    std::string glyph;
    std::string value;
    std::string tooltip;

    bool operator==(const UiOption&) const = default;
  };

  // A single prop value in a declarative UI tree. Numbers are always double
  // (Luau numbers); number arrays carry data series (e.g. graph values), string
  // arrays carry option lists (e.g. select options), and option arrays carry
  // ui.segmented options.
  using UiTreeValue =
      std::variant<bool, double, std::string, std::vector<double>, std::vector<std::string>, std::vector<UiOption>>;

  // One node of a declarative control tree produced by plugin code. Pure data:
  // it crosses the script-worker → UI-thread boundary by value and is mapped to
  // retained `lib/ui/controls/` instances by `UiTreeReconciler`.
  struct UiTreeNode {
    std::string type; // "column", "row", "label", "glyph", ...
    std::string key;  // stable identity for keyed reconciliation ("" = positional)
    std::unordered_map<std::string, UiTreeValue> props;
    std::vector<UiTreeNode> children;

    bool operator==(const UiTreeNode&) const = default;
  };

} // namespace ui
