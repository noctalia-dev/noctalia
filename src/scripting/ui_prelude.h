#pragma once

namespace scripting {

  // Pure-Luau `ui.*` vocabulary, executed in every plugin thread before the
  // entry script loads. Constructors are plain table-taggers — the host reads
  // the resulting tree from desktopWidget.render(); no per-control C binding.
  // `key` inside props gives a child a stable identity for keyed reconciliation.
  inline constexpr const char* kUiPrelude = R"luau(
ui = {}
local function ctor(t)
  return function(props, children)
    return { type = t, props = props or {}, children = children or {} }
  end
end
ui.column = ctor("column")
ui.row = ctor("row")
ui.box = ctor("box")
ui.label = ctor("label")
ui.glyph = ctor("glyph")
ui.image = ctor("image")
ui.separator = ctor("separator")
ui.spacer = ctor("spacer")
ui.progress = ctor("progress")
ui.button = ctor("button")
ui.graph = ctor("graph")
ui.input = ctor("input")
ui.select = ctor("select")
ui.slider = ctor("slider")
ui.toggle = ctor("toggle")
ui.scroll = ctor("scroll")
)luau";

} // namespace scripting
