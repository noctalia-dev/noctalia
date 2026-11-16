#include "config/toml.h"
#include "lua.h"
#include "luacode.h"
#include "lualib.h"
#include "scripting/plugin_bindings.h"
#include "scripting/ui_handler_table.h"
#include "scripting/ui_prelude.h"
#include "tests/test_check.h"

#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

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
  TEST_CHECK(state != nullptr);
  luaL_openlibs(state);

  scripting::PluginBindingContext context;
  context.ownerId = "test/plugin:panel";
  scripting::ScriptSettings settings;
  settings.emplace("output_glyphs", WidgetSettingStringMap{{"eDP-1", "laptop"}, {"DP-1", "monitor"}});
  context.settings = &settings;
  scripting::registerPluginBindings(state, &context);
  lua_pushcfunction(state, scripting::luau_getConfig, "getConfig");
  lua_setglobal(state, "getConfig");

  TEST_CHECK(runLuau(
      state, "=string-map-config",
      "local glyphs = getConfig('output_glyphs')\n"
      "assert(type(glyphs) == 'table')\n"
      "assert(glyphs['eDP-1'] == 'laptop')\n"
      "assert(glyphs['DP-1'] == 'monitor')\n"
  ));

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
  const auto checkConfigSetting = [&](std::string_view path, std::string_view assertion) {
    scripting::pushConfigSetting(state, config, path);
    lua_setglobal(state, "v");
    TEST_CHECK(runLuau(state, "=config-setting", assertion));
  };
  checkConfigSetting("shell.offline_mode", "assert(v == true)");
  checkConfigSetting("shell.time_format", "assert(v == '%H:%M')");
  checkConfigSetting("shell.answer", "assert(v == 42)");
  checkConfigSetting("shell.scale", "assert(math.abs(v - 1.25) < 0.000001)");
  checkConfigSetting("bar.order", "assert(#v == 2 and v[1] == 'a' and v[2] == 'b')");
  checkConfigSetting("shell.nested", "assert(v.name == 'section' and v.enabled == false)");
  checkConfigSetting("bar.order[0]", "assert(v == 'a')");
  checkConfigSetting("shell.nope", "assert(v == nil)");
  TEST_CHECK(runLuau(state, "=ui-prelude", scripting::kUiPrelude));
  TEST_CHECK(runLuau(state, "=empty-accepts", "panel.render(ui.dropZone({ accepts = {} }))"));
  TEST_CHECK(context.patch.uiTree.has_value());
  const auto acceptsProp = context.patch.uiTree->props.find("accepts");
  TEST_CHECK(acceptsProp != context.patch.uiTree->props.end());
  const auto* strings = std::get_if<std::vector<std::string>>(&acceptsProp->second);
  TEST_CHECK(strings != nullptr);
  TEST_CHECK(strings != nullptr && strings->empty());
  TEST_CHECK(std::get_if<std::vector<double>>(&acceptsProp->second) == nullptr);

  context.patch = {};
  TEST_CHECK(runLuau(state, "=empty-graph", "panel.render(ui.graph({ values = {} }))"));
  TEST_CHECK(context.patch.uiTree.has_value());
  const auto valuesProp = context.patch.uiTree->props.find("values");
  TEST_CHECK(valuesProp != context.patch.uiTree->props.end());
  const auto* numbers = std::get_if<std::vector<double>>(&valuesProp->second);
  TEST_CHECK(numbers != nullptr && numbers->empty());

  // `key` is lifted out of the props into the node's identity, and only a string
  // counts as one. It is read ahead of the other props now that it names the
  // node's handlers, so the outcome is pinned here.
  for (const auto& [source, expectedKey] :
       {std::pair{"panel.render(ui.label({ key = 'k', text = 'x' }))", "k"},
        std::pair{"panel.render(ui.label({ key = 7, text = 'x' }))", ""},
        std::pair{"panel.render(ui.label({ text = 'x' }))", ""}, std::pair{"panel.render(ui.label())", ""}}) {
    context.patch = {};
    TEST_CHECK(runLuau(state, "=key-extraction", source));
    TEST_CHECK(context.patch.uiTree.has_value());
    TEST_CHECK(context.patch.uiTree->key == expectedKey);
    TEST_CHECK(!context.patch.uiTree->props.contains("key"));
  }

  // A function-valued callback prop is registered in the render's handler table
  // and the tree carries the generated name in its place.
  context.patch = {};
  TEST_CHECK(runLuau(
      state, "=closure-handlers",
      "clicked = ''\n"
      "panel.render(ui.column({}, {\n"
      "  ui.button({ key = 'a', onClick = function() clicked = 'a' end }),\n"
      "  ui.button({ key = 'b', onClick = function() clicked = 'b' end }),\n"
      "}))"
  ));
  TEST_CHECK(context.patch.uiTree.has_value());
  std::string firstRenderName;
  TEST_CHECK(context.patch.uiTree->children.size() == 2);
  for (const auto& [child, expected] : {std::pair{0, "a"}, std::pair{1, "b"}}) {
    const auto& button = context.patch.uiTree->children[static_cast<std::size_t>(child)];
    const auto it = button.props.find("onClick");
    TEST_CHECK(it != button.props.end());
    const auto* name = std::get_if<std::string>(&it->second);
    TEST_CHECK(name != nullptr);
    // Distinct keys must yield distinct names, or one button would fire the
    // other's closure.
    TEST_CHECK(name->contains(expected));
    if (child == 0) {
      firstRenderName = *name;
    }
    // The name resolves to the closure that produced it.
    lua_getglobal(state, scripting::kUiHandlerTable);
    TEST_CHECK(lua_istable(state, -1));
    lua_getfield(state, -1, name->c_str());
    TEST_CHECK(lua_isfunction(state, -1));
    TEST_CHECK(lua_pcall(state, 0, 0, 0) == 0);
    lua_pop(state, 1);
    const auto fired = [&] {
      lua_getglobal(state, "clicked");
      const std::string value = lua_isstring(state, -1) ? lua_tostring(state, -1) : "";
      lua_pop(state, 1);
      return value;
    }();
    TEST_CHECK(fired == expected);
  }

  // Re-rendering the same keyed tree reuses the handler names, so an otherwise
  // unchanged tree still compares equal and skips reconciliation.
  context.patch = {};
  TEST_CHECK(runLuau(
      state, "=closure-handlers-rerender",
      "panel.render(ui.column({}, {\n"
      "  ui.button({ key = 'a', onClick = function() clicked = 'a2' end }),\n"
      "}))"
  ));
  TEST_CHECK(context.patch.uiTree.has_value() && !context.patch.uiTree->children.empty());
  const auto& rerenderedButton = context.patch.uiTree->children.front();
  const auto onClick = rerenderedButton.props.find("onClick");
  const auto* rerenderedName =
      onClick != rerenderedButton.props.end() ? std::get_if<std::string>(&onClick->second) : nullptr;
  TEST_CHECK(rerenderedName != nullptr && *rerenderedName == firstRenderName);
  // The superseded render's handlers are gone: the table was replaced, not
  // merged, so the second button's name no longer resolves.
  lua_getglobal(state, scripting::kUiHandlerTable);
  lua_getfield(state, -1, "@ui:/0/b#onClick");
  TEST_CHECK(lua_isnil(state, -1));
  lua_pop(state, 2);

  // Registration is per prop, not per node: nothing is special-cased to
  // onClick, so every callback prop on one node gets its own handler.
  context.patch = {};
  TEST_CHECK(runLuau(
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
  ));
  TEST_CHECK(context.patch.uiTree.has_value() && !context.patch.uiTree->children.empty());
  const auto& multiButton = context.patch.uiTree->children.front();
  for (const auto& [prop, expected] :
       {std::pair{"onClick", "left"}, std::pair{"onRightClick", "right"}, std::pair{"onHover", "hover:true"}}) {
    const auto it = multiButton.props.find(prop);
    const auto* name = it != multiButton.props.end() ? std::get_if<std::string>(&it->second) : nullptr;
    TEST_CHECK(name != nullptr);
    // Sibling props on one node must not collide, or a right click would run
    // the left-click closure.
    TEST_CHECK(name->ends_with(std::string("#") + prop));
    lua_getglobal(state, scripting::kUiHandlerTable);
    lua_getfield(state, -1, name->c_str());
    TEST_CHECK(lua_isfunction(state, -1));
    // onHover is handed the state string the host would send on enter.
    lua_pushstring(state, "true");
    TEST_CHECK(lua_pcall(state, 1, 0, 0) == 0);
    lua_pop(state, 1);
    lua_getglobal(state, "fired");
    const std::string fired = lua_isstring(state, -1) ? lua_tostring(state, -1) : "";
    lua_pop(state, 1);
    TEST_CHECK(fired == expected);
  }

  // Native context menus are accepted only from a direct pointer callback.
  context.beginCall({});
  TEST_CHECK(runLuau(
      state, "=context-menu-no-pointer",
      "assert(panel.openContextMenu({\n"
      "  items = {{ id = 'copy', label = 'Copy' }},\n"
      "  onActivate = 'activateMenu',\n"
      "}) == false)"
  ));
  TEST_CHECK(!context.contextMenuRequest.has_value());

  scripting::ScriptSnapshot pointerSnapshot;
  pointerSnapshot.pointerContext = scripting::ScriptPointerContext{.x = 42.5F, .y = 17.0F, .serial = 99, .time = 123};
  context.beginCall(pointerSnapshot);
  TEST_CHECK(runLuau(
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
  ));
  const auto& menu = context.contextMenuRequest;
  TEST_CHECK(menu.has_value());
  TEST_CHECK(menu->items.size() == 4);
  TEST_CHECK(menu->onActivate == "activateMenu");
  TEST_CHECK(menu->maxVisible == 4);
  TEST_CHECK(menu->pointer.serial == 99 && menu->pointer.x == 42.5F);
  const auto* number = menu->context.has_value() ? std::get_if<double>(&*menu->context) : nullptr;
  TEST_CHECK(number != nullptr && *number == 7.0);
  TEST_CHECK(
      menu->items.front().kind == scripting::ScriptContextMenuItemKind::Header
      && menu->items[1].kind == scripting::ScriptContextMenuItemKind::Action
      && menu->items[1].id == "copy"
      && !menu->items[2].enabled
      && menu->items[3].kind == scripting::ScriptContextMenuItemKind::Separator
  );

  context.beginCall(pointerSnapshot);
  TEST_CHECK(!runLuau(
      state, "=context-menu-duplicate-id",
      "panel.openContextMenu({ items = {\n"
      "  { id = 'same', label = 'One' }, { id = 'same', label = 'Two' },\n"
      "}, onActivate = 'activateMenu' })"
  ));
  context.beginCall(pointerSnapshot);
  TEST_CHECK(!runLuau(
      state, "=context-menu-no-action",
      "panel.openContextMenu({ items = { { kind = 'separator' } }, onActivate = 'activateMenu' })"
  ));
  context.beginCall(pointerSnapshot);
  TEST_CHECK(!runLuau(
      state, "=context-menu-invalid-context",
      "panel.openContextMenu({ items = {{ id = 'x', label = 'X' }}, onActivate = 'activateMenu', context = {} "
      "})"
  ));

  lua_close(state);
  return 0;
}
