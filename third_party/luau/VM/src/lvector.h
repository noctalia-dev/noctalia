// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "lobject.h"

#define sizevector() (sizeof(LuauVector))

LUAI_FUNC LuauVector* luaVec_newvector(lua_State* L, double x, double y, double z, double w);
LUAI_FUNC void luaVec_freevector(lua_State* L, LuauVector* v, struct lua_Page* page);
