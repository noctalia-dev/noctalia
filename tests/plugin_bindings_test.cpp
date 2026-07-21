#include "lua.h"
#include "luacode.h"
#include "lualib.h"
#include "scripting/plugin_bindings.h"
#include "scripting/ui_prelude.h"

#include <cstdio>
#include <cstdlib>
#include <print>
#include <string>
#include <string_view>
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

  lua_close(state);
  return ok ? 0 : 1;
}
