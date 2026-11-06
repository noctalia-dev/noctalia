// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "lualib.h"

#include "lstate.h"
#include "lapi.h"
#include "ldo.h"
#include "lgc.h"
#include "ludata.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

LUAU_FASTFLAGVARIABLE(LuauFastpcall)
LUAU_FASTFLAGVARIABLE(LuauPcallOptimize)

static void writestring(const char* s, size_t l)
{
    fwrite(s, 1, l, stdout);
}

static int luaB_print(lua_State* L)
{
    int n = lua_gettop(L); // number of arguments
    for (int i = 1; i <= n; i++)
    {
        size_t l;
        const char* s = luaL_tolstring(L, i, &l); // convert to string using __tostring et al
        if (i > 1)
            writestring("\t", 1);
        writestring(s, l);
        lua_pop(L, 1); // pop result
    }
    writestring("\n", 1);
    return 0;
}

static int luaB_tonumber(lua_State* L)
{
    int base = luaL_optinteger(L, 2, 10);
    if (base == 10)
    { // standard conversion
        int isnum = 0;
        double n = lua_tonumberx(L, 1, &isnum);
        if (isnum)
        {
            lua_pushnumber(L, n);
            return 1;
        }
        luaL_checkany(L, 1); // error if we don't have any argument
    }
    else
    {
        const char* s1 = luaL_checkstring(L, 1);
        luaL_argcheck(L, 2 <= base && base <= 36, 2, "base out of range");
        char* s2;
        unsigned long long n;
        n = strtoull(s1, &s2, base);
        if (s1 != s2)
        { // at least one valid digit?
            while (isspace((unsigned char)(*s2)))
                s2++; // skip trailing spaces
            if (*s2 == '\0')
            { // no invalid trailing characters?
                lua_pushnumber(L, (double)n);
                return 1;
            }
        }
    }
    lua_pushnil(L); // else not a number
    return 1;
}

static int luaB_error(lua_State* L)
{
    int level = luaL_optinteger(L, 2, 1);
    lua_settop(L, 1);
    if (lua_isstring(L, 1) && level > 0)
    { // add extra information?
        luaL_where(L, level);
        lua_pushvalue(L, 1);
        lua_concat(L, 2);
    }
    lua_error(L);
}

static int luaB_getmetatable(lua_State* L)
{
    luaL_checkany(L, 1);
    if (!lua_getmetatable(L, 1))
    {
        lua_pushnil(L);
        return 1; // no metatable
    }
    luaL_getmetafield(L, 1, "__metatable");
    return 1; // returns either __metatable field (if present) or metatable
}

static int luaB_setmetatable(lua_State* L)
{
    int t = lua_type(L, 2);
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_argexpected(L, t == LUA_TNIL || t == LUA_TTABLE, 2, "nil or table");
    if (luaL_getmetafield(L, 1, "__metatable"))
        luaL_error(L, "cannot change a protected metatable");
    lua_settop(L, 2);
    lua_setmetatable(L, 1);
    return 1;
}

static void getfunc(lua_State* L, int opt)
{
    if (lua_isfunction(L, 1))
        lua_pushvalue(L, 1);
    else
    {
        lua_Debug ar;
        int level = opt ? luaL_optinteger(L, 1, 1) : luaL_checkinteger(L, 1);
        luaL_argcheck(L, level >= 0, 1, "level must be non-negative");
        if (lua_getinfo(L, level, "f", &ar) == 0)
            luaL_argerror(L, 1, "invalid level");
        if (lua_isnil(L, -1))
            luaL_error(L, "no function environment for tail call at level %d", level);
    }
}

static int luaB_getfenv(lua_State* L)
{
    getfunc(L, 1);
    if (lua_iscfunction(L, -1))             // is a C function?
        lua_pushvalue(L, LUA_GLOBALSINDEX); // return the thread's global env.
    else
        lua_getfenv(L, -1);
    lua_setsafeenv(L, -1, false);
    return 1;
}

static int luaB_setfenv(lua_State* L)
{
    luaL_checktype(L, 2, LUA_TTABLE);
    getfunc(L, 0);
    lua_pushvalue(L, 2);
    lua_setsafeenv(L, -1, false);
    if (lua_isnumber(L, 1) && lua_tonumber(L, 1) == 0)
    {
        // change environment of current thread
        lua_pushthread(L);
        lua_insert(L, -2);
        lua_setfenv(L, -2);
        return 0;
    }
    else if (lua_iscfunction(L, -2) || lua_setfenv(L, -2) == 0)
        luaL_error(L, "'setfenv' cannot change environment of given object");
    return 1;
}

static int luaB_rawequal(lua_State* L)
{
    luaL_checkany(L, 1);
    luaL_checkany(L, 2);
    lua_pushboolean(L, lua_rawequal(L, 1, 2));
    return 1;
}

static int luaB_rawget(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checkany(L, 2);
    lua_settop(L, 2);
    lua_rawget(L, 1);
    return 1;
}

static int luaB_rawset(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checkany(L, 2);
    luaL_checkany(L, 3);
    lua_settop(L, 3);
    lua_rawset(L, 1);
    return 1;
}

static int luaB_rawlen(lua_State* L)
{
    int tt = lua_type(L, 1);
    luaL_argcheck(L, tt == LUA_TTABLE || tt == LUA_TSTRING, 1, "table or string expected");
    int len = lua_objlen(L, 1);
    lua_pushinteger(L, len);
    return 1;
}

static int luaB_gcinfo(lua_State* L)
{
    lua_pushinteger(L, lua_gc(L, LUA_GCCOUNT, 0));
    return 1;
}

static int luaB_type(lua_State* L)
{
    luaL_checkany(L, 1);
    // resulting name doesn't differentiate between userdata types
    lua_pushstring(L, lua_typename(L, lua_type(L, 1)));
    return 1;
}

static int luaB_typeof(lua_State* L)
{
    luaL_checkany(L, 1);
    // resulting name returns __type if specified unless the input is a newproxy-created userdata
    lua_pushstring(L, luaL_typename(L, 1));
    return 1;
}

int luaB_next(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 2); // create a 2nd argument if there isn't one
    if (lua_next(L, 1))
        return 2;
    else
    {
        lua_pushnil(L);
        return 1;
    }
}

static int luaB_pairs(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushvalue(L, lua_upvalueindex(1)); // return generator,
    lua_pushvalue(L, 1);                   // state,
    lua_pushnil(L);                        // and initial value
    return 3;
}

int luaB_inext(lua_State* L)
{
    int i = luaL_checkinteger(L, 2);
    luaL_checktype(L, 1, LUA_TTABLE);
    i++; // next value
    lua_pushinteger(L, i);
    lua_rawgeti(L, 1, i);
    return (lua_isnil(L, -1)) ? 0 : 2;
}

static int luaB_ipairs(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushvalue(L, lua_upvalueindex(1)); // return generator,
    lua_pushvalue(L, 1);                   // state,
    lua_pushinteger(L, 0);                 // and initial value
    return 3;
}

static int luaB_assert(lua_State* L)
{
    luaL_checkany(L, 1);
    if (!lua_toboolean(L, 1))
        luaL_error(L, "%s", luaL_optstring(L, 2, "assertion failed!"));
    return lua_gettop(L);
}

static int luaB_select(lua_State* L)
{
    int n = lua_gettop(L);
    if (lua_type(L, 1) == LUA_TSTRING && *lua_tostring(L, 1) == '#')
    {
        lua_pushinteger(L, n - 1);
        return 1;
    }
    else
    {
        int i = luaL_checkinteger(L, 1);
        if (i < 0)
            i = n + i;
        else if (i > n)
            i = n;
        luaL_argcheck(L, 1 <= i, 1, "index out of range");
        return n - i;
    }
}

static int luaB_pcally(lua_State* L)
{
    luaL_checkany(L, 1);

    return lua_pcallyieldable(L, lua_gettop(L) - 1, LUA_MULTRET, 0);
}

static int luaB_pcallcont(lua_State* L, int status)
{
    if (FFlag::LuauPcallOptimize)
    {
        luaD_checkstack(L, 1);

        if (status == 0)
        {
            StkId base = L->base;
            StkId top = L->top;

            // Move results 1 right to make space for the 'true' status
            for (StkId q = top; q > base; q--)
                setobj2s(L, q, q - 1);

            setbvalue(base, 1);
            L->top = top + 1;

            return cast_int(L->top - base);
        }
        else
        {
            StkId top = L->top;

            // Move error 1 right to make space for the 'false' status
            setobj2s(L, top, top - 1);

            setbvalue(top - 1, 0);
            L->top = top + 1;

            return 2;
        }
    }
    else
    {
        if (status == 0)
        {
            lua_rawcheckstack(L, 1);
            lua_pushboolean(L, true);
            lua_insert(L, 1); // insert status before all results
            return lua_gettop(L);
        }
        else
        {
            lua_rawcheckstack(L, 1);
            lua_pushboolean(L, false);
            lua_insert(L, -2); // insert status before error object
            return 2;
        }
    }
}

static int luaB_xpcally(lua_State* L)
{
    luaL_checktype(L, 2, LUA_TFUNCTION);

    // swap function & error function
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_replace(L, 1);
    lua_replace(L, 2);
    // at this point the stack looks like err, f, args

    return lua_pcallyieldable(L, lua_gettop(L) - 2, LUA_MULTRET, 1);
}

static int luaB_xpcallcont(lua_State* L, int status)
{
    if (status == 0)
    {
        if (FFlag::LuauPcallOptimize)
        {
            StkId base = L->base;

            // xpcall had an 'errfunc' before the results, so we just replace it with 'true' status
            setbvalue(base, 1);

            return cast_int(L->top - base);
        }
        else
        {
            lua_rawcheckstack(L, 1);
            lua_pushboolean(L, true);
            lua_replace(L, 1);    // replace error function with status
            return lua_gettop(L); // return status + all results
        }
    }
    else
    {
        if (FFlag::LuauPcallOptimize)
        {
            luaD_checkstack(L, 1);

            StkId top = L->top;

            // Move error 1 right to make space for the 'false' status
            setobj2s(L, top, top - 1);

            setbvalue(top - 1, 0);
            L->top = top + 1;

            return 2;
        }
        else
        {
            lua_rawcheckstack(L, 1);
            lua_pushboolean(L, false);
            lua_insert(L, -2); // place status before the error that was on top of the stack
            return 2;
        }
    }
}

static int luaB_tostring(lua_State* L)
{
    luaL_checkany(L, 1);
    luaL_tolstring(L, 1, NULL);
    return 1;
}

static int luaB_newproxy(lua_State* L)
{
    int t = lua_type(L, 1);
    luaL_argexpected(L, t == LUA_TNONE || t == LUA_TNIL || t == LUA_TBOOLEAN, 1, "nil or boolean");

    bool needsmt = lua_toboolean(L, 1);

    lua_newuserdatatagged(L, 0, UTAG_PROXY);

    if (needsmt)
    {
        lua_newtable(L);
        lua_setmetatable(L, -2);
    }

    return 1;
}

static const luaL_Reg base_funcs[] = {
    {"assert", luaB_assert},
    {"error", luaB_error},
    {"gcinfo", luaB_gcinfo},
    {"getfenv", luaB_getfenv},
    {"getmetatable", luaB_getmetatable},
    {"next", luaB_next},
    {"newproxy", luaB_newproxy},
    {"print", luaB_print},
    {"rawequal", luaB_rawequal},
    {"rawget", luaB_rawget},
    {"rawset", luaB_rawset},
    {"rawlen", luaB_rawlen},
    {"select", luaB_select},
    {"setfenv", luaB_setfenv},
    {"setmetatable", luaB_setmetatable},
    {"tonumber", luaB_tonumber},
    {"tostring", luaB_tostring},
    {"type", luaB_type},
    {"typeof", luaB_typeof},
    {NULL, NULL},
};

static void auxopen(lua_State* L, const char* name, lua_CFunction f, lua_CFunction u)
{
    lua_pushcfunction(L, u, NULL);
    lua_pushcclosure(L, f, name, 1);
    lua_setfield(L, -2, name);
}

int luaopen_base(lua_State* L)
{
    // set global _G
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    lua_setglobal(L, "_G");

    // open lib into global table
    luaL_register(L, "_G", base_funcs);
    lua_pushliteral(L, "Luau");
    lua_setglobal(L, "_VERSION"); // set global _VERSION

    // `ipairs' and `pairs' need auxiliary functions as upvalues
    auxopen(L, "ipairs", luaB_ipairs, luaB_inext);
    auxopen(L, "pairs", luaB_pairs, luaB_next);

    lua_pushcclosurek(L, luaB_pcally, "pcall", 0, luaB_pcallcont);

    if (FFlag::LuauFastpcall)
        L->global->builtinPcall = clvalue(luaA_toobject(L, -1));

    lua_setfield(L, -2, "pcall");

    lua_pushcclosurek(L, luaB_xpcally, "xpcall", 0, luaB_xpcallcont);

    if (FFlag::LuauFastpcall)
        L->global->builtinXpcall = clvalue(luaA_toobject(L, -1));

    lua_setfield(L, -2, "xpcall");

    return 1;
}
