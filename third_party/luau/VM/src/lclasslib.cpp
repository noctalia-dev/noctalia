#include "lapi.h"
#include "lobject.h"
#include "lua.h"
#include "lualib.h"
#include "lstate.h"

/**
 * class.isinstance(obj: any, class: class): boolean
 */
static int class_isinstance(lua_State* L)
{
    luaL_checkany(L, 1);
    luaL_checktype(L, 2, LUA_TCLASS);

    const TValue* obj = luaA_toobject(L, 1);

    if (!ttisobject(obj))
    {
        lua_pushboolean(L, false);
        return 1;
    }

    const TValue* cls = luaA_toobject(L, 2);

    const LuauClass* lclass = classvalue(cls);

    for (LuauClass* objClass = objectvalue(obj)->lclass; objClass != NULL; objClass = objClass->super)
    {
        if (objClass == lclass)
        {
            lua_pushboolean(L, true);
            return 1;
        }
    }

    lua_pushboolean(L, false);
    return 1;
}

static int class_classof(lua_State* L)
{
    luaL_checkany(L, 1);
    if (!lua_isobject(L, 1))
    {
        lua_pushnil(L);
        return 1;
    }
    const TValue* inst = luaA_toobject(L, 1);
    const LuauObject* ci = objectvalue(inst);
    luaA_pushclass(L, ci->lclass);
    return 1;
}

static const luaL_Reg classlib[] = {
    {"isinstance", class_isinstance},
    {"classof", class_classof},
    {nullptr, nullptr},
};

/*
** Open class library
*/
int luaopen_class(lua_State* L)
{
    luaL_register(L, LUA_CLASSLIBNAME, classlib);
    return 1;
}