#pragma once

#include "scripting/script_runtime_types.h"

#include <string>
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

    // Entry id ("author/plugin:entry") for diagnostics, e.g. an undeclared getConfig.
    std::string ownerId;

    void beginCall(ScriptSnapshot nextSnapshot) {
      snapshot = std::move(nextSnapshot);
      patch = {};
      sideEffects.clear();
    }
  };

  void registerPluginBindings(lua_State* L, PluginBindingContext* context);

  // noctalia.getConfig(key) binding — reads the runtime's seeded settings for every
  // entry kind (widget/shortcut/service/etc.).
  int luau_getConfig(lua_State* L);

} // namespace scripting
