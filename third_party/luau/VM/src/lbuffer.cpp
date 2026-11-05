// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "lbuffer.h"

#include "lgc.h"
#include "lmem.h"

#include <string.h>

LUAU_FASTFLAG(LuauBufferCage)

Buffer* luaB_newbuffer(lua_State* L, size_t s)
{
    if (s > MAX_BUFFER_SIZE)
        luaM_toobig(L);

    global_State* g = L->global;
    Buffer* b = (FFlag::LuauBufferCage && g->cagealloc) ? luaM_newgcocaged(L, Buffer, sizebuffer(s), L->activememcat, LUA_TBUFFER)
                                                         : luaM_newgco(L, Buffer, sizebuffer(s), L->activememcat, LUA_TBUFFER);

    luaC_init(L, b, LUA_TBUFFER);
    b->len = unsigned(s);
    memset(b->data, 0, b->len);
    return b;
}

void luaB_freebuffer(lua_State* L, Buffer* b, lua_Page* page)
{
    global_State* g = L->global;
    if (FFlag::LuauBufferCage && g->cagealloc)
        luaM_freegcocaged(L, b, sizebuffer(b->len), b->memcat, page);
    else
        luaM_freegco(L, b, sizebuffer(b->len), b->memcat, page);
}
