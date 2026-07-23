#include "scripting/plugin_bindings.h"

#include "core/log.h"
#include "lua.h"
#include "lualib.h"
#include "ui/ui_tree.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

  constexpr Logger kLog("plugin-bindings");
  constexpr const char* kWidgetKey = "__plugin_binding_context";

  scripting::PluginBindingContext* getContext(lua_State* L) {
    lua_getglobal(L, kWidgetKey);
    auto* context = static_cast<scripting::PluginBindingContext*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return context;
  }

  std::string_view optionalStringArg(lua_State* L, int index) {
    if (lua_gettop(L) < index || lua_isnil(L, index)) {
      return {};
    }
    return luaL_checkstring(L, index);
  }

  int luau_setText(lua_State* L) {
    size_t len = 0;
    const char* text = luaL_checklstring(L, 1, &len);
    if (auto* context = getContext(L)) {
      context->patch.text = std::string(text, len);
    }
    return 0;
  }

  int luau_setGlyph(lua_State* L) {
    size_t len = 0;
    const char* name = luaL_checklstring(L, 1, &len);
    if (auto* context = getContext(L)) {
      context->patch.glyph = std::string(name, len);
      context->patch.image.reset();
    }
    return 0;
  }

  int luau_setImage(lua_State* L) {
    size_t len = 0;
    const char* path = luaL_checklstring(L, 1, &len);
    const bool watch = lua_gettop(L) >= 2 && !lua_isnil(L, 2) && lua_toboolean(L, 2) != 0;
    float width = 0.0f;
    float height = 0.0f;
    if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) {
      width = static_cast<float>(luaL_checknumber(L, 3));
      height = width;
    }
    if (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) {
      height = static_cast<float>(luaL_checknumber(L, 4));
    }
    if (auto* context = getContext(L)) {
      context->patch.image =
          scripting::ScriptImagePatch{.path = std::string(path, len), .watch = watch, .width = width, .height = height};
      context->patch.glyph.reset();
    }
    return 0;
  }

  std::string tableOptionalStringField(lua_State* L, int tableIndex, const char* key) {
    lua_getfield(L, tableIndex, key);
    std::string out;
    if (lua_isstring(L, -1)) {
      size_t len = 0;
      const char* value = lua_tolstring(L, -1, &len);
      out.assign(value, len);
    }
    lua_pop(L, 1);
    return out;
  }

  std::optional<std::string> tableStringField(lua_State* L, int tableIndex, const char* key) {
    lua_getfield(L, tableIndex, key);
    std::optional<std::string> out;
    if (lua_isstring(L, -1)) {
      size_t len = 0;
      const char* value = lua_tolstring(L, -1, &len);
      out = std::string(value, len);
    }
    lua_pop(L, 1);
    return out;
  }

  std::string tableOptionalStringIndex(lua_State* L, int tableIndex, int rawIndex) {
    lua_rawgeti(L, tableIndex, rawIndex);
    std::string out;
    if (lua_isstring(L, -1)) {
      size_t len = 0;
      const char* value = lua_tolstring(L, -1, &len);
      out.assign(value, len);
    }
    lua_pop(L, 1);
    return out;
  }

  scripting::ScriptTooltipRowPatch tooltipRowFromLuaTable(lua_State* L, int rowIndex) {
    scripting::ScriptTooltipRowPatch row;
    row.key = tableOptionalStringField(L, rowIndex, "key");
    row.value = tableOptionalStringField(L, rowIndex, "value");
    if (row.key.empty()) {
      row.key = tableOptionalStringIndex(L, rowIndex, 1);
    }
    if (row.value.empty()) {
      row.value = tableOptionalStringIndex(L, rowIndex, 2);
    }
    return row;
  }

  int luau_setTooltip(lua_State* L) {
    scripting::ScriptTooltipPatch patch;
    if (lua_isnoneornil(L, 1)) {
      patch.clear = true;
    } else if (lua_isstring(L, 1)) {
      size_t len = 0;
      const char* text = lua_tolstring(L, 1, &len);
      patch.text.assign(text, len);
      patch.clear = patch.text.empty();
    } else if (lua_istable(L, 1)) {
      auto singleRow = tooltipRowFromLuaTable(L, 1);
      if (!singleRow.key.empty() || !singleRow.value.empty()) {
        patch.rows.push_back(std::move(singleRow));
      } else {
        const int rowCount = lua_objlen(L, 1);
        patch.rows.reserve(static_cast<std::size_t>(std::max(0, rowCount)));
        for (int i = 1; i <= rowCount; ++i) {
          lua_rawgeti(L, 1, i);
          if (lua_istable(L, -1)) {
            auto row = tooltipRowFromLuaTable(L, lua_gettop(L));
            if (!row.key.empty() || !row.value.empty()) {
              patch.rows.push_back(std::move(row));
            }
          }
          lua_pop(L, 1);
        }
      }
      patch.clear = patch.rows.empty();
    } else {
      luaL_argerror(L, 1, "expected string, row table, or nil");
      return 0;
    }

    if (auto* context = getContext(L)) {
      context->patch.tooltip = std::move(patch);
    }
    return 0;
  }

  int luau_clearTooltip(lua_State* L) {
    if (auto* context = getContext(L)) {
      scripting::ScriptTooltipPatch patch;
      patch.clear = true;
      context->patch.tooltip = std::move(patch);
    }
    return 0;
  }

  int luau_setFont(lua_State* L) {
    size_t len = 0;
    const char* family = luaL_checklstring(L, 1, &len);
    if (auto* context = getContext(L)) {
      context->patch.fontFamily = std::string(family, len);
      const std::string_view baseline = optionalStringArg(L, 2);
      if (!baseline.empty()) {
        context->patch.fontBaseline = std::string(baseline);
      }
    }
    return 0;
  }

  int luau_setColor(lua_State* L) {
    size_t len = 0;
    const char* role = luaL_checklstring(L, 1, &len);
    if (auto* context = getContext(L)) {
      context->patch.textColor =
          scripting::ScriptColorPatch{.role = std::string(role, len), .mode = std::string(optionalStringArg(L, 2))};
    }
    return 0;
  }

  int luau_setGlyphColor(lua_State* L) {
    size_t len = 0;
    const char* role = luaL_checklstring(L, 1, &len);
    if (auto* context = getContext(L)) {
      context->patch.glyphColor =
          scripting::ScriptColorPatch{.role = std::string(role, len), .mode = std::string(optionalStringArg(L, 2))};
    }
    return 0;
  }

  int luau_isVertical(lua_State* L) {
    auto* context = getContext(L);
    lua_pushboolean(L, context != nullptr && context->snapshot.isVertical ? 1 : 0);
    return 1;
  }

  int luau_outputName(lua_State* L) {
    auto* context = getContext(L);
    if (context == nullptr || context->snapshot.outputName.empty()) {
      lua_pushnil(L);
      return 1;
    }
    lua_pushlstring(L, context->snapshot.outputName.data(), context->snapshot.outputName.size());
    return 1;
  }

  int luau_setVisible(lua_State* L) {
    bool visible = lua_toboolean(L, 1) != 0;
    if (auto* context = getContext(L)) {
      context->patch.visible = visible;
    }
    return 0;
  }

  // Shared render(tree) binding for bar widgets, desktop widgets, and panels
  // (defined after readUiTreeNode below).
  int luau_ui_render(lua_State* L);

  const luaL_Reg kWidgetLib[] = {
      {"setText", luau_setText},
      {"setGlyph", luau_setGlyph},
      {"setImage", luau_setImage},
      {"setTooltip", luau_setTooltip},
      {"clearTooltip", luau_clearTooltip},
      {"setFont", luau_setFont},
      {"setColor", luau_setColor},
      {"setGlyphColor", luau_setGlyphColor},
      {"isVertical", luau_isVertical},
      {"outputName", luau_outputName},
      {"setVisible", luau_setVisible},
      {"render", luau_ui_render},
      {nullptr, nullptr},
  };

  // ── shortcut.* — control-center quick-toggle tile presentation ──

  int luau_shortcut_setLabel(lua_State* L) {
    size_t len = 0;
    const char* label = luaL_checklstring(L, 1, &len);
    if (auto* context = getContext(L)) {
      context->patch.label = std::string(label, len);
    }
    return 0;
  }

  int luau_shortcut_setIcon(lua_State* L) {
    size_t onLen = 0;
    const char* on = luaL_checklstring(L, 1, &onLen);
    if (auto* context = getContext(L)) {
      context->patch.iconOn = std::string(on, onLen);
      // Optional second arg: a distinct "off" icon; defaults to the same glyph.
      if (lua_gettop(L) >= 2 && lua_isstring(L, 2)) {
        size_t offLen = 0;
        const char* off = lua_tolstring(L, 2, &offLen);
        context->patch.iconOff = std::string(off, offLen);
      } else {
        context->patch.iconOff = std::string(on, onLen);
      }
    }
    return 0;
  }

  int luau_shortcut_setActive(lua_State* L) {
    const bool active = lua_toboolean(L, 1) != 0;
    if (auto* context = getContext(L)) {
      context->patch.active = active;
    }
    return 0;
  }

  int luau_shortcut_setEnabled(lua_State* L) {
    const bool enabled = lua_toboolean(L, 1) != 0;
    if (auto* context = getContext(L)) {
      context->patch.enabled = enabled;
    }
    return 0;
  }

  const luaL_Reg kShortcutLib[] = {
      {"setLabel", luau_shortcut_setLabel},
      {"setIcon", luau_shortcut_setIcon},
      {"setActive", luau_shortcut_setActive},
      {"setEnabled", luau_shortcut_setEnabled},
      {nullptr, nullptr},
  };

  // ── launcher.* — launcher-provider results ──

  // launcher.setResults(query, results) — replaces this provider's result set.
  // `query` echoes the text passed to onQuery so late async results map back to the
  // right query. Each result is a table { id, title, subtitle?, glyph?, icon?,
  // badge?, category?, presentation?, score?, query? }. `category` matches a label
  // declared by a [[launcher_provider.category]] manifest entry, letting the
  // launcher's category filter bar narrow this provider's results. A result's
  // optional `query` rewrites the input to this provider's prefix + that sub-query
  // when activated (staying in the provider), the declarative form of setQuery. An
  // empty array clears the provider's results.
  int luau_launcher_setResults(lua_State* L) {
    size_t queryLen = 0;
    const char* query = luaL_checklstring(L, 1, &queryLen);
    luaL_checktype(L, 2, LUA_TTABLE);
    auto* context = getContext(L);
    if (context == nullptr) {
      return 0;
    }
    scripting::ScriptLauncherResultSet set;
    set.query.assign(query, queryLen);
    const int count = lua_objlen(L, 2);
    set.results.reserve(static_cast<std::size_t>(std::max(0, count)));
    for (int i = 1; i <= count; ++i) {
      lua_rawgeti(L, 2, i);
      if (lua_istable(L, -1)) {
        const int row = lua_gettop(L);
        scripting::ScriptLauncherResult result;
        result.id = tableOptionalStringField(L, row, "id");
        result.title = tableOptionalStringField(L, row, "title");
        result.subtitle = tableOptionalStringField(L, row, "subtitle");
        result.glyph = tableOptionalStringField(L, row, "glyph");
        result.icon = tableOptionalStringField(L, row, "icon");
        result.badge = tableOptionalStringField(L, row, "badge");
        result.category = tableOptionalStringField(L, row, "category");
        result.presentation = tableOptionalStringField(L, row, "presentation");
        result.query = tableStringField(L, row, "query");
        lua_getfield(L, row, "score");
        if (lua_isnumber(L, -1)) {
          result.score = lua_tonumber(L, -1);
        }
        lua_pop(L, 1);
        if (!result.id.empty() || !result.title.empty()) {
          set.results.push_back(std::move(result));
        }
      }
      lua_pop(L, 1);
    }
    context->patch.launcherResults = std::move(set);
    return 0;
  }

  // launcher.setQuery(text) — sets the launcher input to this provider's query.
  // The host prepends the provider's resolved prefix, so `text` is the sub-query
  // after the prefix (e.g. setQuery("fruits ")) and the panel stays in this provider
  // regardless of the configured prefix. setQuery("") returns to the provider root.
  int luau_launcher_setQuery(lua_State* L) {
    size_t queryLen = 0;
    const char* query = luaL_checklstring(L, 1, &queryLen);
    if (auto* context = getContext(L)) {
      context->patch.launcherQuery = std::string(query, queryLen);
    }
    return 0;
  }

  const luaL_Reg kLauncherLib[] = {
      {"setResults", luau_launcher_setResults},
      {"setQuery", luau_launcher_setQuery},
      {nullptr, nullptr},
  };

  // ── desktopWidget.* — declarative UI tree + tick opt-ins ──

  constexpr int kUiTreeMaxDepth = 32;
  constexpr int kUiTreeMaxChildren = 256;

  // Reads the value at `index` into a UiTreeValue. A table is read as a number
  // array (graph data) or a string array (select options) — its element type is
  // decided by the first element and must be uniform; any other shape is rejected.
  bool readUiTreeValue(lua_State* L, int index, std::string_view propName, ui::UiTreeValue& out) {
    switch (lua_type(L, index)) {
    case LUA_TBOOLEAN:
      out = lua_toboolean(L, index) != 0;
      return true;
    case LUA_TNUMBER:
      out = lua_tonumber(L, index);
      return true;
    case LUA_TSTRING: {
      size_t len = 0;
      const char* value = lua_tolstring(L, index, &len);
      out = std::string(value, len);
      return true;
    }
    case LUA_TTABLE: {
      const int count = lua_objlen(L, index);
      // An empty Luau table is shapeless while UiTreeValue holds exactly one
      // array type, so the element type must be decided here at read time:
      // empty tables default to graph data (numbers), except the drop-zone
      // `accepts` prop, whose contract is a string array (empty = accepts
      // nothing) and which would otherwise fail its type validation.
      lua_rawgeti(L, index, 1);
      const bool stringArray = (count == 0 && propName == "accepts") || (count > 0 && lua_type(L, -1) == LUA_TSTRING);
      lua_pop(L, 1);
      if (stringArray) {
        std::vector<std::string> strings;
        strings.reserve(static_cast<std::size_t>(count));
        for (int i = 1; i <= count; ++i) {
          lua_rawgeti(L, index, i);
          if (lua_type(L, -1) != LUA_TSTRING) {
            lua_pop(L, 1);
            return false;
          }
          size_t len = 0;
          const char* value = lua_tolstring(L, -1, &len);
          strings.emplace_back(value, len);
          lua_pop(L, 1);
        }
        out = std::move(strings);
        return true;
      }
      std::vector<double> numbers;
      numbers.reserve(static_cast<std::size_t>(std::max(0, count)));
      for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, index, i);
        if (!lua_isnumber(L, -1)) {
          lua_pop(L, 1);
          return false;
        }
        numbers.push_back(lua_tonumber(L, -1));
        lua_pop(L, 1);
      }
      out = std::move(numbers);
      return true;
    }
    default:
      return false;
    }
  }

  // Recursively reads a ui.* node table { type, props, children } at `index`.
  // Malformed input is loud: the offending node/prop is logged and skipped.
  bool readUiTreeNode(lua_State* L, int index, ui::UiTreeNode& out, int depth, const std::string& ownerId) {
    if (depth > kUiTreeMaxDepth) {
      kLog.warn("plugin {}: ui tree deeper than {} levels, subtree dropped", ownerId, kUiTreeMaxDepth);
      return false;
    }
    if (!lua_istable(L, index)) {
      kLog.warn("plugin {}: ui tree node is not a table", ownerId);
      return false;
    }
    const int node = lua_absindex(L, index);

    out.type = tableOptionalStringField(L, node, "type");
    if (out.type.empty()) {
      kLog.warn("plugin {}: ui tree node without a type, dropped", ownerId);
      return false;
    }

    lua_getfield(L, node, "props");
    if (lua_istable(L, -1)) {
      const int props = lua_gettop(L);
      lua_pushnil(L);
      while (lua_next(L, props) != 0) {
        if (lua_isstring(L, -2)) {
          size_t keyLen = 0;
          const char* key = lua_tolstring(L, -2, &keyLen);
          ui::UiTreeValue value;
          const std::string_view propName(key, keyLen);
          if (readUiTreeValue(L, -1, propName, value)) {
            out.props.emplace(std::string(key, keyLen), std::move(value));
          } else {
            kLog.warn("plugin {}: ui node '{}' prop '{}' has an unsupported value type", ownerId, out.type, key);
          }
        }
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);

    if (auto it = out.props.find("key"); it != out.props.end()) {
      if (const auto* key = std::get_if<std::string>(&it->second)) {
        out.key = *key;
      }
      out.props.erase(it);
    }

    lua_getfield(L, node, "children");
    if (lua_istable(L, -1)) {
      const int children = lua_gettop(L);
      const int count = std::min(lua_objlen(L, children), kUiTreeMaxChildren);
      if (lua_objlen(L, children) > kUiTreeMaxChildren) {
        kLog.warn(
            "plugin {}: ui node '{}' has more than {} children, extra dropped", ownerId, out.type, kUiTreeMaxChildren
        );
      }
      out.children.reserve(static_cast<std::size_t>(std::max(0, count)));
      for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, children, i);
        ui::UiTreeNode child;
        if (readUiTreeNode(L, lua_gettop(L), child, depth + 1, ownerId)) {
          out.children.push_back(std::move(child));
        }
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);

    return true;
  }

  // render(tree) — replaces the entry's declarative control tree. Shared by
  // barWidget.render, desktopWidget.render, and panel.render.
  int luau_ui_render(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    auto* context = getContext(L);
    if (context == nullptr) {
      return 0;
    }
    ui::UiTreeNode tree;
    if (readUiTreeNode(L, 1, tree, 0, context->ownerId)) {
      context->patch.uiTree = std::move(tree);
    }
    return 0;
  }

  int luau_desktop_setWantsSecondTicks(lua_State* L) {
    const bool wants = lua_toboolean(L, 1) != 0;
    if (auto* context = getContext(L)) {
      context->patch.wantsSecondTicks = wants;
    }
    return 0;
  }

  int luau_desktop_setNeedsFrameTick(lua_State* L) {
    const bool needs = lua_toboolean(L, 1) != 0;
    if (auto* context = getContext(L)) {
      context->patch.needsFrameTick = needs;
    }
    return 0;
  }

  const luaL_Reg kDesktopWidgetLib[] = {
      {"render", luau_ui_render},
      {"setWantsSecondTicks", luau_desktop_setWantsSecondTicks},
      {"setNeedsFrameTick", luau_desktop_setNeedsFrameTick},
      {nullptr, nullptr},
  };

  // ── panel.* — declarative UI tree for a [[panel]] entry ──

  // panel.close() — request the host close this panel.
  int luau_panel_close(lua_State* L) {
    if (auto* context = getContext(L)) {
      context->patch.requestClose = true;
    }
    return 0;
  }

  int luau_panel_setWantsSecondTicks(lua_State* L) {
    const bool wants = lua_toboolean(L, 1) != 0;
    if (auto* context = getContext(L)) {
      context->patch.wantsSecondTicks = wants;
    }
    return 0;
  }

  const luaL_Reg kPanelLib[] = {
      {"render", luau_ui_render},
      {"close", luau_panel_close},
      {"setWantsSecondTicks", luau_panel_setWantsSecondTicks},
      {nullptr, nullptr},
  };

} // namespace

namespace scripting {

  int luau_getConfig(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    auto* context = getContext(L);
    if (context == nullptr || context->settings == nullptr) {
      lua_pushnil(L);
      return 1;
    }

    auto it = context->settings->find(key);
    if (it == context->settings->end()) {
      kLog.warn("plugin {} read undeclared setting '{}'", context->ownerId, key);
      lua_pushnil(L);
      return 1;
    }

    std::visit(
        [L](const auto& val) {
          using T = std::decay_t<decltype(val)>;
          if constexpr (std::is_same_v<T, bool>)
            lua_pushboolean(L, val ? 1 : 0);
          else if constexpr (std::is_same_v<T, std::int64_t>)
            lua_pushnumber(L, static_cast<double>(val));
          else if constexpr (std::is_same_v<T, double>)
            lua_pushnumber(L, val);
          else if constexpr (std::is_same_v<T, std::string>)
            lua_pushlstring(L, val.data(), val.size());
          else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            lua_createtable(L, static_cast<int>(val.size()), 0);
            for (size_t i = 0; i < val.size(); ++i) {
              lua_pushlstring(L, val[i].data(), val[i].size());
              lua_rawseti(L, -2, static_cast<int>(i + 1));
            }
          } else if constexpr (std::is_same_v<T, WidgetSettingStringMap>) {
            lua_createtable(L, 0, static_cast<int>(val.size()));
            for (const auto& [mapKey, value] : val) {
              lua_pushlstring(L, value.data(), value.size());
              lua_setfield(L, -2, mapKey.c_str());
            }
          } else {
            lua_pushnil(L);
          }
        },
        it->second
    );
    return 1;
  }

  void registerPluginBindings(lua_State* L, PluginBindingContext* context) {
    lua_pushlightuserdata(L, context);
    lua_setglobal(L, kWidgetKey);

    luaL_register(L, "barWidget", kWidgetLib);
    lua_pop(L, 1);
    luaL_register(L, "shortcut", kShortcutLib);
    lua_pop(L, 1);
    luaL_register(L, "launcher", kLauncherLib);
    lua_pop(L, 1);
    luaL_register(L, "desktopWidget", kDesktopWidgetLib);
    lua_pop(L, 1);
    luaL_register(L, "panel", kPanelLib);
    lua_pop(L, 1);
  }

} // namespace scripting
