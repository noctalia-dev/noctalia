#include "core/toml.h"
#include "lua.h"
#include "luacode.h"
#include "lualib.h"
#include "scripting/plugin_bindings.h"
#include "scripting/ui_handler_table.h"
#include "scripting/ui_prelude.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "plugin_bindings_test: {}", message);
    }
    return condition;
  }

  bool runLuau(lua_State* state, std::string_view chunkName, std::string_view source) {
    std::size_t bytecodeSize = 0;
    char* bytecode = luau_compile(source.data(), source.size(), nullptr, &bytecodeSize);
    if (bytecode == nullptr) {
      return false;
    }

    const std::string name(chunkName);
    const int loadResult = luau_load(state, name.c_str(), bytecode, bytecodeSize, 0);
    std::free(bytecode);
    if (loadResult != 0) {
      lua_pop(state, 1);
      return false;
    }
    if (lua_pcall(state, 0, 0, 0) != 0) {
      lua_pop(state, 1);
      return false;
    }
    return true;
  }

} // namespace

int main() {
  lua_State* state = luaL_newstate();
  if (!expect(state != nullptr, "failed to create Luau state")) {
    return 1;
  }
  luaL_openlibs(state);

  scripting::PluginBindingContext context;
  context.ownerId = "test/plugin:panel";
  scripting::ScriptSettings settings;
  settings.emplace("output_glyphs", WidgetSettingStringMap{{"eDP-1", "laptop"}, {"DP-1", "monitor"}});
  context.settings = &settings;
  scripting::registerPluginBindings(state, &context);
  lua_pushcfunction(state, scripting::luau_getConfig, "getConfig");
  lua_setglobal(state, "getConfig");

  bool ok = true;
  ok = expect(
           runLuau(
               state, "=string-map-config",
               "local glyphs = getConfig('output_glyphs')\n"
               "assert(type(glyphs) == 'table')\n"
               "assert(glyphs['eDP-1'] == 'laptop')\n"
               "assert(glyphs['DP-1'] == 'monitor')\n"
           ),
           "getConfig should expose string maps as associative tables"
       )
      && ok;

  const auto config = toml::parse(R"(
[shell]
offline_mode = true
time_format = "%H:%M"
answer = 42
scale = 1.25

[shell.nested]
name = "section"
enabled = false

[bar]
order = ["a", "b"]
)");
  const auto checkConfigSetting = [&](std::string_view path, std::string_view assertion, const char* message) {
    scripting::pushConfigSetting(state, config, path);
    lua_setglobal(state, "v");
    return expect(runLuau(state, "=config-setting", assertion), message);
  };
  ok = checkConfigSetting("shell.offline_mode", "assert(v == true)", "getSetting should expose booleans") && ok;
  ok = checkConfigSetting("shell.time_format", "assert(v == '%H:%M')", "getSetting should expose exact strings") && ok;
  ok = checkConfigSetting("shell.answer", "assert(v == 42)", "getSetting should expose integers") && ok;
  ok = checkConfigSetting(
           "shell.scale", "assert(math.abs(v - 1.25) < 0.000001)", "getSetting should expose floating-point values"
       )
      && ok;
  ok = checkConfigSetting(
           "bar.order", "assert(#v == 2 and v[1] == 'a' and v[2] == 'b')",
           "getSetting should expose arrays as one-based Lua tables"
       )
      && ok;
  ok =
      checkConfigSetting(
          "shell.nested", "assert(v.name == 'section' and v.enabled == false)", "getSetting should expose nested tables"
      )
      && ok;
  ok = checkConfigSetting(
           "bar.order[0]", "assert(v == 'a')", "getSetting paths should use zero-based TOML array indices"
       )
      && ok;
  ok = checkConfigSetting("shell.nope", "assert(v == nil)", "getSetting should return nil for missing paths") && ok;
  ok = expect(runLuau(state, "=ui-prelude", scripting::kUiPrelude), "failed to execute production UI prelude") && ok;
  ok = expect(
           runLuau(state, "=empty-accepts", "panel.render(ui.dropZone({ accepts = {} }))"),
           "failed to render DropZone with an empty accepts list"
       )
      && ok;
  ok = expect(context.patch.uiTree.has_value(), "panel.render should produce a UI tree") && ok;
  if (context.patch.uiTree.has_value()) {
    const auto it = context.patch.uiTree->props.find("accepts");
    ok = expect(it != context.patch.uiTree->props.end(), "DropZone accepts prop should survive deserialization") && ok;
    if (it != context.patch.uiTree->props.end()) {
      const auto* strings = std::get_if<std::vector<std::string>>(&it->second);
      ok = expect(strings != nullptr, "empty DropZone accepts should deserialize as a string array") && ok;
      ok = expect(strings != nullptr && strings->empty(), "empty DropZone accepts should remain empty") && ok;
      ok = expect(
               std::get_if<std::vector<double>>(&it->second) == nullptr,
               "empty DropZone accepts must not deserialize as graph data"
           )
          && ok;
    }
  }

  context.patch = {};
  ok = expect(
           runLuau(state, "=empty-graph", "panel.render(ui.graph({ values = {} }))"),
           "failed to render Graph with empty values"
       )
      && ok;
  ok = expect(context.patch.uiTree.has_value(), "graph render should produce a UI tree") && ok;
  if (context.patch.uiTree.has_value()) {
    const auto it = context.patch.uiTree->props.find("values");
    ok = expect(it != context.patch.uiTree->props.end(), "Graph values prop should survive deserialization") && ok;
    if (it != context.patch.uiTree->props.end()) {
      const auto* numbers = std::get_if<std::vector<double>>(&it->second);
      ok = expect(numbers != nullptr && numbers->empty(), "other empty arrays should remain numeric graph data") && ok;
    }
  }

  // `key` is lifted out of the props into the node's identity, and only a string
  // counts as one. It is read ahead of the other props now that it names the
  // node's handlers, so the outcome is pinned here.
  for (const auto& [source, expectedKey] :
       {std::pair{"panel.render(ui.label({ key = 'k', text = 'x' }))", "k"},
        std::pair{"panel.render(ui.label({ key = 7, text = 'x' }))", ""},
        std::pair{"panel.render(ui.label({ text = 'x' }))", ""}, std::pair{"panel.render(ui.label())", ""}}) {
    context.patch = {};
    ok = expect(runLuau(state, "=key-extraction", source), "failed to render a node for key extraction") && ok;
    if (!expect(context.patch.uiTree.has_value(), "key extraction render should produce a UI tree")) {
      ok = false;
      continue;
    }
    ok = expect(context.patch.uiTree->key == expectedKey, "only a string key should become the node key") && ok;
    ok = expect(!context.patch.uiTree->props.contains("key"), "key should never remain a prop") && ok;
  }

  // A function-valued callback prop is registered in the render's handler table
  // and the tree carries the generated name in its place.
  context.patch = {};
  ok = expect(
           runLuau(
               state, "=closure-handlers",
               "clicked = ''\n"
               "panel.render(ui.column({}, {\n"
               "  ui.button({ key = 'a', onClick = function() clicked = 'a' end }),\n"
               "  ui.button({ key = 'b', onClick = function() clicked = 'b' end }),\n"
               "}))"
           ),
           "failed to render a tree with closure callbacks"
       )
      && ok;
  ok = expect(context.patch.uiTree.has_value(), "closure render should produce a UI tree") && ok;
  std::string firstRenderName;
  const bool twoButtons =
      context.patch.uiTree.has_value() && expect(context.patch.uiTree->children.size() == 2, "expected two buttons");
  ok = twoButtons && ok;
  if (twoButtons) {
    for (const auto& [child, expected] : {std::pair{0, "a"}, std::pair{1, "b"}}) {
      const auto& button = context.patch.uiTree->children[static_cast<std::size_t>(child)];
      const auto it = button.props.find("onClick");
      ok = expect(it != button.props.end(), "a closure onClick should reach the tree as a prop") && ok;
      if (it == button.props.end()) {
        continue;
      }
      const auto* name = std::get_if<std::string>(&it->second);
      ok = expect(name != nullptr, "a closure onClick should be carried as a generated name") && ok;
      if (name == nullptr) {
        continue;
      }
      // Distinct keys must yield distinct names, or one button would fire the
      // other's closure.
      ok = expect(name->contains(expected), "a handler name should carry the node key") && ok;
      if (child == 0) {
        firstRenderName = *name;
      }
      // The name resolves to the closure that produced it.
      lua_getglobal(state, scripting::kUiHandlerTable);
      ok = expect(lua_istable(state, -1), "render should publish a handler table") && ok;
      lua_getfield(state, -1, name->c_str());
      ok = expect(lua_isfunction(state, -1), "a generated name should resolve to the registered closure") && ok;
      if (lua_isfunction(state, -1)) {
        ok = expect(lua_pcall(state, 0, 0, 0) == 0, "a registered closure should be callable") && ok;
      } else {
        lua_pop(state, 1);
      }
      lua_pop(state, 1);
      const auto fired = [&] {
        lua_getglobal(state, "clicked");
        const std::string value = lua_isstring(state, -1) ? lua_tostring(state, -1) : "";
        lua_pop(state, 1);
        return value;
      }();
      ok = expect(fired == expected, "calling a handler should run that node's own closure") && ok;
    }
  }

  // Re-rendering the same keyed tree reuses the handler names, so an otherwise
  // unchanged tree still compares equal and skips reconciliation.
  context.patch = {};
  ok = expect(
           runLuau(
               state, "=closure-handlers-rerender",
               "panel.render(ui.column({}, {\n"
               "  ui.button({ key = 'a', onClick = function() clicked = 'a2' end }),\n"
               "}))"
           ),
           "failed to re-render closure callbacks"
       )
      && ok;
  const bool rerendered = context.patch.uiTree.has_value()
      && expect(!context.patch.uiTree->children.empty(), "re-render should produce a button");
  ok = rerendered && ok;
  if (rerendered) {
    const auto& button = context.patch.uiTree->children.front();
    const auto it = button.props.find("onClick");
    const auto* name = it != button.props.end() ? std::get_if<std::string>(&it->second) : nullptr;
    ok = expect(name != nullptr && *name == firstRenderName, "a keyed node should keep its handler name") && ok;
    // The superseded render's handlers are gone: the table was replaced, not
    // merged, so the second button's name no longer resolves.
    lua_getglobal(state, scripting::kUiHandlerTable);
    lua_getfield(state, -1, "@ui:/0/b#onClick");
    ok = expect(lua_isnil(state, -1), "a dropped node's handler should not survive the next render") && ok;
    lua_pop(state, 2);
  }

  // Registration is per prop, not per node: nothing is special-cased to
  // onClick, so every callback prop on one node gets its own handler.
  context.patch = {};
  ok = expect(
           runLuau(
               state, "=closure-handlers-multi",
               "fired = ''\n"
               "panel.render(ui.row({}, {\n"
               "  ui.button({\n"
               "    key = 'x',\n"
               "    onClick = function() fired = 'left' end,\n"
               "    onRightClick = function() fired = 'right' end,\n"
               "    onHover = function(state) fired = 'hover:' .. state end,\n"
               "  }),\n"
               "}))"
           ),
           "failed to render a node with several closure callbacks"
       )
      && ok;
  const bool multiRendered = context.patch.uiTree.has_value() && !context.patch.uiTree->children.empty();
  ok = expect(multiRendered, "multi-callback render should produce a button") && ok;
  if (multiRendered) {
    const auto& button = context.patch.uiTree->children.front();
    for (const auto& [prop, expected] :
         {std::pair{"onClick", "left"}, std::pair{"onRightClick", "right"}, std::pair{"onHover", "hover:true"}}) {
      const auto it = button.props.find(prop);
      const auto* name = it != button.props.end() ? std::get_if<std::string>(&it->second) : nullptr;
      ok = expect(name != nullptr, "every closure callback prop should reach the tree as a generated name") && ok;
      if (name == nullptr) {
        continue;
      }
      // Sibling props on one node must not collide, or a right click would run
      // the left-click closure.
      ok = expect(name->ends_with(std::string("#") + prop), "a handler name should carry its prop") && ok;
      lua_getglobal(state, scripting::kUiHandlerTable);
      lua_getfield(state, -1, name->c_str());
      const bool callable = expect(lua_isfunction(state, -1), "each callback prop should resolve to its own closure");
      ok = callable && ok;
      if (callable) {
        // onHover is handed the state string the host would send on enter.
        lua_pushstring(state, "true");
        ok = expect(lua_pcall(state, 1, 0, 0) == 0, "a registered closure should be callable") && ok;
      } else {
        lua_pop(state, 1);
      }
      lua_pop(state, 1);
      lua_getglobal(state, "fired");
      const std::string fired = lua_isstring(state, -1) ? lua_tostring(state, -1) : "";
      lua_pop(state, 1);
      ok = expect(fired == expected, "each callback prop should run its own closure") && ok;
    }
  }

  // Native context menus are accepted only from a direct pointer callback.
  context.beginCall({});
  ok = expect(
           runLuau(
               state, "=context-menu-no-pointer",
               "assert(panel.openContextMenu({\n"
               "  items = {{ id = 'copy', label = 'Copy' }},\n"
               "  onActivate = 'activateMenu',\n"
               "}) == false)"
           ),
           "openContextMenu should return false without pointer context"
       )
      && ok;
  ok = expect(!context.contextMenuRequest.has_value(), "a failed context menu open should not publish a request") && ok;

  scripting::ScriptSnapshot pointerSnapshot;
  pointerSnapshot.pointerContext = scripting::ScriptPointerContext{.x = 42.5F, .y = 17.0F, .serial = 99, .time = 123};
  context.beginCall(pointerSnapshot);
  ok = expect(
           runLuau(
               state, "=context-menu-valid",
               "assert(panel.openContextMenu({\n"
               "  items = {\n"
               "    { kind = 'header', label = 'Clipboard' },\n"
               "    { kind = 'item', id = 'copy', label = 'Copy' },\n"
               "    { id = 'delete', label = 'Delete', enabled = false },\n"
               "    { kind = 'separator' },\n"
               "  },\n"
               "  onActivate = 'activateMenu', context = 7, maxVisible = 4,\n"
               "}) == true)"
           ),
           "openContextMenu should accept a valid request from a pointer callback"
       )
      && ok;
  const auto& menu = context.contextMenuRequest;
  ok = expect(menu.has_value(), "a valid context menu should publish a one-shot request") && ok;
  if (menu.has_value()) {
    ok = expect(menu->items.size() == 4, "context menu should retain every item") && ok;
    ok = expect(menu->onActivate == "activateMenu", "context menu should retain its callback") && ok;
    ok = expect(menu->maxVisible == 4, "context menu should retain maxVisible") && ok;
    ok = expect(menu->pointer.serial == 99 && menu->pointer.x == 42.5F, "context menu should retain pointer metadata")
        && ok;
    const auto* number = menu->context.has_value() ? std::get_if<double>(&*menu->context) : nullptr;
    ok = expect(number != nullptr && *number == 7.0, "context menu should retain scalar context") && ok;
    ok = expect(
             menu->items.front().kind == scripting::ScriptContextMenuItemKind::Header
                 && menu->items[1].kind == scripting::ScriptContextMenuItemKind::Action
                 && menu->items[1].id == "copy"
                 && !menu->items[2].enabled
                 && menu->items[3].kind == scripting::ScriptContextMenuItemKind::Separator,
             "context menu should preserve item kinds, ids, and enabled state"
         )
        && ok;
  }

  context.beginCall(pointerSnapshot);
  ok = expect(
           !runLuau(
               state, "=context-menu-duplicate-id",
               "panel.openContextMenu({ items = {\n"
               "  { id = 'same', label = 'One' }, { id = 'same', label = 'Two' },\n"
               "}, onActivate = 'activateMenu' })"
           ),
           "openContextMenu should reject duplicate ids"
       )
      && ok;
  context.beginCall(pointerSnapshot);
  ok = expect(
           !runLuau(
               state, "=context-menu-no-action",
               "panel.openContextMenu({ items = { { kind = 'separator' } }, onActivate = 'activateMenu' })"
           ),
           "openContextMenu should reject a menu without an enabled action"
       )
      && ok;
  context.beginCall(pointerSnapshot);
  ok = expect(
           !runLuau(
               state, "=context-menu-invalid-context",
               "panel.openContextMenu({ items = {{ id = 'x', label = 'X' }}, onActivate = 'activateMenu', context = {} "
               "})"
           ),
           "openContextMenu should reject non-scalar context"
       )
      && ok;

  lua_close(state);
  return ok ? 0 : 1;
}
