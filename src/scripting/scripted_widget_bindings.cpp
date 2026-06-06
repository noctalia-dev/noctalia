#include "scripting/scripted_widget_bindings.h"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

  constexpr const char* kWidgetKey = "__scripted_widget";

  scripting::ScriptedWidgetBindingContext* getContext(lua_State* L) {
    lua_getglobal(L, kWidgetKey);
    auto* context = static_cast<scripting::ScriptedWidgetBindingContext*>(lua_touserdata(L, -1));
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
      context->patch.image = scripting::ScriptWidgetImagePatch{
          .path = std::string(path, len), .watch = watch, .width = width, .height = height
      };
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

  scripting::ScriptWidgetTooltipRowPatch tooltipRowFromLuaTable(lua_State* L, int rowIndex) {
    scripting::ScriptWidgetTooltipRowPatch row;
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
    scripting::ScriptWidgetTooltipPatch patch;
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
      scripting::ScriptWidgetTooltipPatch patch;
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
    }
    return 0;
  }

  int luau_setColor(lua_State* L) {
    size_t len = 0;
    const char* role = luaL_checklstring(L, 1, &len);
    if (auto* context = getContext(L)) {
      context->patch.textColor = scripting::ScriptWidgetColorPatch{
          .role = std::string(role, len), .mode = std::string(optionalStringArg(L, 2))
      };
    }
    return 0;
  }

  int luau_setGlyphColor(lua_State* L) {
    size_t len = 0;
    const char* role = luaL_checklstring(L, 1, &len);
    if (auto* context = getContext(L)) {
      context->patch.glyphColor = scripting::ScriptWidgetColorPatch{
          .role = std::string(role, len), .mode = std::string(optionalStringArg(L, 2))
      };
    }
    return 0;
  }

  int luau_isVertical(lua_State* L) {
    auto* context = getContext(L);
    lua_pushboolean(L, context != nullptr && context->snapshot.isVertical ? 1 : 0);
    return 1;
  }

  int luau_setUpdateInterval(lua_State* L) {
    auto ms = static_cast<float>(luaL_checknumber(L, 1));
    if (auto* context = getContext(L)) {
      context->patch.updateIntervalMs = std::max(16, static_cast<int>(ms));
    }
    return 0;
  }

  int luau_setVisible(lua_State* L) {
    bool visible = lua_toboolean(L, 1) != 0;
    if (auto* context = getContext(L)) {
      context->patch.visible = visible;
    }
    return 0;
  }

  int luau_getConfig(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    auto* context = getContext(L);
    if (context == nullptr || context->settings == nullptr) {
      lua_pushnil(L);
      return 1;
    }

    auto it = context->settings->find(key);
    if (it == context->settings->end()) {
      if (lua_gettop(L) >= 2) {
        lua_pushvalue(L, 2);
        return 1;
      }
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
          } else {
            lua_pushnil(L);
          }
        },
        it->second
    );
    return 1;
  }

  // ── Manifest parsing (barWidget.define) ──────────────────────────────────

  std::string tableStringField(lua_State* L, int tableIndex, const char* key, std::string fallback = {}) {
    lua_getfield(L, tableIndex, key);
    std::string out = lua_isstring(L, -1) ? std::string(lua_tostring(L, -1)) : std::move(fallback);
    lua_pop(L, 1);
    return out;
  }

  bool tableBoolField(lua_State* L, int tableIndex, const char* key, bool fallback) {
    lua_getfield(L, tableIndex, key);
    bool out = lua_isnil(L, -1) ? fallback : (lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);
    return out;
  }

  std::optional<double> tableNumberField(lua_State* L, int tableIndex, const char* key) {
    lua_getfield(L, tableIndex, key);
    std::optional<double> out;
    if (lua_isnumber(L, -1)) {
      out = lua_tonumber(L, -1);
    }
    lua_pop(L, 1);
    return out;
  }

  scripting::ManifestFieldType parseFieldType(std::string_view type) {
    if (type == "bool" || type == "boolean") {
      return scripting::ManifestFieldType::Bool;
    }
    if (type == "int" || type == "integer") {
      return scripting::ManifestFieldType::Int;
    }
    if (type == "double" || type == "number" || type == "float") {
      return scripting::ManifestFieldType::Double;
    }
    if (type == "select" || type == "enum") {
      return scripting::ManifestFieldType::Select;
    }
    if (type == "file") {
      return scripting::ManifestFieldType::File;
    }
    if (type == "folder") {
      return scripting::ManifestFieldType::Folder;
    }
    if (type == "glyph") {
      return scripting::ManifestFieldType::Glyph;
    }
    if (type == "color") {
      return scripting::ManifestFieldType::Color;
    }
    return scripting::ManifestFieldType::String;
  }

  void parseFieldDefault(lua_State* L, int fieldIndex, scripting::ManifestField& field) {
    lua_getfield(L, fieldIndex, "default");
    switch (field.type) {
    case scripting::ManifestFieldType::Bool:
      field.boolDefault = lua_toboolean(L, -1) != 0;
      break;
    case scripting::ManifestFieldType::Int:
    case scripting::ManifestFieldType::Double:
      field.numberDefault = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0;
      break;
    default:
      field.stringDefault = lua_isstring(L, -1) ? std::string(lua_tostring(L, -1)) : std::string{};
      break;
    }
    lua_pop(L, 1);
  }

  void parseFieldOptions(lua_State* L, int fieldIndex, scripting::ManifestField& field) {
    lua_getfield(L, fieldIndex, "options");
    if (lua_istable(L, -1)) {
      const int optionsIndex = lua_gettop(L);
      const int count = lua_objlen(L, optionsIndex);
      for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, optionsIndex, i);
        if (lua_istable(L, -1)) {
          const int optIndex = lua_gettop(L);
          scripting::ManifestSelectOption opt;
          opt.value = tableStringField(L, optIndex, "value");
          opt.label = tableStringField(L, optIndex, "label", opt.value);
          if (!opt.value.empty()) {
            field.options.push_back(std::move(opt));
          }
        } else if (lua_isstring(L, -1)) {
          std::string value = lua_tostring(L, -1);
          field.options.push_back(scripting::ManifestSelectOption{.value = value, .label = value});
        }
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }

  void parseFieldExtensions(lua_State* L, int fieldIndex, scripting::ManifestField& field) {
    lua_getfield(L, fieldIndex, "extensions");
    if (lua_istable(L, -1)) {
      const int extensionsIndex = lua_gettop(L);
      const int count = lua_objlen(L, extensionsIndex);
      for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, extensionsIndex, i);
        if (lua_isstring(L, -1)) {
          field.extensions.emplace_back(lua_tostring(L, -1));
        }
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }

  void parseFieldVisibility(lua_State* L, int fieldIndex, scripting::ManifestField& field) {
    lua_getfield(L, fieldIndex, "visible_when");
    if (lua_istable(L, -1)) {
      const int visIndex = lua_gettop(L);
      scripting::ManifestVisibility vis;
      vis.key = tableStringField(L, visIndex, "key");
      lua_getfield(L, visIndex, "values");
      if (lua_istable(L, -1)) {
        const int valuesIndex = lua_gettop(L);
        const int count = lua_objlen(L, valuesIndex);
        for (int i = 1; i <= count; ++i) {
          lua_rawgeti(L, valuesIndex, i);
          if (lua_isstring(L, -1)) {
            vis.values.emplace_back(lua_tostring(L, -1));
          } else if (lua_isboolean(L, -1)) {
            vis.values.emplace_back(lua_toboolean(L, -1) != 0 ? "true" : "false");
          }
          lua_pop(L, 1);
        }
      }
      lua_pop(L, 1);
      if (!vis.key.empty() && !vis.values.empty()) {
        field.visibleWhen = std::move(vis);
      }
    }
    lua_pop(L, 1);
  }

  void parseManifest(lua_State* L, int tableIndex, scripting::ScriptWidgetManifest& manifest) {
    manifest.label = tableStringField(L, tableIndex, "label");
    manifest.version = tableStringField(L, tableIndex, "version");
    manifest.icon = tableStringField(L, tableIndex, "icon");
    manifest.description = tableStringField(L, tableIndex, "description");
    manifest.pickable = tableBoolField(L, tableIndex, "pickable", true);

    lua_getfield(L, tableIndex, "settings");
    if (lua_istable(L, -1)) {
      const int settingsIndex = lua_gettop(L);
      const int count = lua_objlen(L, settingsIndex);
      for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, settingsIndex, i);
        if (lua_istable(L, -1)) {
          const int fieldIndex = lua_gettop(L);
          scripting::ManifestField field;
          field.key = tableStringField(L, fieldIndex, "key");
          if (!field.key.empty()) {
            field.type = parseFieldType(tableStringField(L, fieldIndex, "type", "string"));
            field.label = tableStringField(L, fieldIndex, "label", field.key);
            field.description = tableStringField(L, fieldIndex, "description");
            field.advanced = tableBoolField(L, fieldIndex, "advanced", false);
            field.minValue = tableNumberField(L, fieldIndex, "min");
            field.maxValue = tableNumberField(L, fieldIndex, "max");
            if (auto step = tableNumberField(L, fieldIndex, "step"); step.has_value()) {
              field.step = *step;
            }
            parseFieldDefault(L, fieldIndex, field);
            parseFieldOptions(L, fieldIndex, field);
            parseFieldExtensions(L, fieldIndex, field);
            parseFieldVisibility(L, fieldIndex, field);
            manifest.settings.push_back(std::move(field));
          }
        }
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }

  int luau_define(lua_State* L) {
    auto* context = getContext(L);
    if (context != nullptr && context->manifestOut != nullptr && lua_istable(L, 1)) {
      *context->manifestOut = {};
      parseManifest(L, 1, *context->manifestOut);
      context->defineCalled = true;
    }
    // Abort the chunk during extraction so no top-level side effects run.
    if (context != nullptr && context->manifestExtractionMode) {
      luaL_error(L, "__manifest_captured");
    }
    return 0;
  }

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
      {"setUpdateInterval", luau_setUpdateInterval},
      {"setVisible", luau_setVisible},
      {"getConfig", luau_getConfig},
      {"define", luau_define},
      {nullptr, nullptr},
  };

} // namespace

namespace scripting {

  void registerScriptedWidgetBindings(lua_State* L, ScriptedWidgetBindingContext* context) {
    lua_pushlightuserdata(L, context);
    lua_setglobal(L, kWidgetKey);

    luaL_register(L, "barWidget", kWidgetLib);
    lua_pop(L, 1);
  }

} // namespace scripting
