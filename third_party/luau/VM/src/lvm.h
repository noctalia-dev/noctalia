// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#pragma once

#include "lobject.h"
#include "ltm.h"

#define tostring(L, o) ((ttype(o) == LUA_TSTRING) || (luaV_tostring(L, o)))

#define tonumber(o, n) (ttype(o) == LUA_TNUMBER || (((o) = luaV_tonumber(o, n)) != NULL))

#define equalobj(L, o1, o2) (ttype(o1) == ttype(o2) && luaV_equalval(L, o1, o2))

LUAI_FUNC int luaV_strcmp(const TString* ls, const TString* rs);
LUAI_FUNC int luaV_lessthan(lua_State* L, const TValue* l, const TValue* r);
LUAI_FUNC int luaV_lessequal(lua_State* L, const TValue* l, const TValue* r);
LUAI_FUNC int luaV_equalval(lua_State* L, const TValue* t1, const TValue* t2);

template<TMS op>
void luaV_doarithimpl(lua_State* L, StkId ra, const TValue* rb, const TValue* rc);

LUAI_FUNC void luaV_dolen(lua_State* L, StkId ra, const TValue* rb);
LUAI_FUNC const TValue* luaV_tonumber(const TValue* obj, TValue* n);
LUAI_FUNC const LUA_VECTOR_TYPE* luaV_tovector(const TValue* obj);
LUAI_FUNC int luaV_tostring(lua_State* L, StkId obj);
LUAI_FUNC void luaV_gettable(lua_State* L, const TValue* t, TValue* key, StkId val);
LUAI_FUNC void luaV_settable(lua_State* L, const TValue* t, TValue* key, StkId val);
LUAI_FUNC void luaV_concat(lua_State* L, int total, int last);
LUAI_FUNC void luaV_getimport(lua_State* L, LuaTable* env, TValue* k, StkId res, uint32_t id, bool propagatenil);
LUAI_FUNC void luaV_prepareFORN(lua_State* L, StkId plimit, StkId pstep, StkId pinit);
LUAI_FUNC void luaV_callTM(lua_State* L, int nparams, int res);
LUAI_FUNC void luaV_tryfuncTM(lua_State* L, StkId func);

LUAI_FUNC void luau_execute(lua_State* L);
LUAI_FUNC void luau_finishop(lua_State* L);
LUAI_FUNC int luau_precall(lua_State* L, struct lua_TValue* func, int nresults);
LUAI_FUNC void luau_pushhandlerci(lua_State* L, StkId funcslot, int errfunc, int nresults);
LUAI_FUNC void luau_poscall(lua_State* L, StkId first);
LUAI_FUNC void luau_pospcallsuccess(lua_State* L);
LUAI_FUNC void luau_callhook(lua_State* L, lua_Hook hook, void* userdata);
