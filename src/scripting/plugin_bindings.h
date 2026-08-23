#pragma once

#include "core/toml.h"
#include "scripting/script_runtime_types.h"

#include <string>
#include <string_view>
#include <vector>

struct lua_State;
class LuauHost;

namespace scripting {

  struct PluginBindingContext {
    const ScriptSettings* settings = nullptr;
    LuauHost* host = nullptr;
    ScriptSnapshot snapshot;
    ScriptPatch patch;
    std::vector<ScriptSideEffect> sideEffects;
    std::optional<ScriptContextMenuRequest> contextMenuRequest;

    // Entry id ("author/plugin:entry") for diagnostics, e.g. an undeclared getConfig.
    std::string ownerId;

    void beginCall(ScriptSnapshot nextSnapshot) {
      snapshot = std::move(nextSnapshot);
      patch = {};
      sideEffects.clear();
      contextMenuRequest.reset();
    }
  };

  void registerPluginBindings(lua_State* L, PluginBindingContext* context);

  // noctalia.getConfig(key) binding — reads the runtime's seeded settings for every
  // entry kind (widget/shortcut/service/etc.).
  int luau_getConfig(lua_State* L);

  // Pushes the value at `path` (toml++ at_path syntax: dotted keys, zero-based array
  // indices, e.g. "shell.offline_mode", "bar.order[0]") from the effective-config
  // snapshot onto the Lua stack; nil when the path resolves to nothing. Returns 1.
  int pushConfigSetting(lua_State* L, const toml::table& config, std::string_view path);

} // namespace scripting
