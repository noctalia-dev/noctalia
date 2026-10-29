// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "lvector.h"

#include "lgc.h"
#include "lmem.h"

LuauVector* luaVec_newvector(lua_State* L, double x, double y, double z, double w)
{
    LuauVector* v = luaM_newgcofixed(L, LuauVector, sizevector(), L->activememcat, LUA_TVECTOR);
    luaC_init(L, v, LUA_TVECTOR);
    v->v[0] = (LUA_VECTOR_TYPE)x;
    v->v[1] = (LUA_VECTOR_TYPE)y;
    v->v[2] = (LUA_VECTOR_TYPE)z;
    condvector4(v->v[3] = (LUA_VECTOR_TYPE)w, (void)w);
    return v;
}

void luaVec_freevector(lua_State* L, LuauVector* v, lua_Page* page)
{
    luaM_freegcofixed(L, v, sizevector(), v->memcat, page);
}
