// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#pragma once

#include "lua.h"

struct lua_Page;
union GCObject;

#define luaM_newgco(L, t, size, memcat, tt) cast_to(t*, luaM_newgco_(L, size, memcat, tt, 0))
#define luaM_newgcoudata(L, t, size, memcat, tt, tag) cast_to(t*, luaM_newgco_(L, size, memcat, tt, tag))
#define luaM_newgcofixed(L, t, size, memcat, tt) cast_to(t*, luaM_newgcofixed_(L, size, memcat, tt))
#define luaM_newgcocaged(L, t, size, memcat, tt) cast_to(t*, luaM_newgcocaged_(L, size, memcat, tt, 0))
#define luaM_freegco(L, p, size, memcat, page) luaM_freegco_(L, obj2gco(p), size, memcat, page)
#define luaM_freegcofixed(L, p, size, memcat, page) luaM_freegcofixed_(L, obj2gco(p), size, memcat, page)
#define luaM_freegcocaged(L, p, size, memcat, page) luaM_freegcocaged_(L, obj2gco(p), size, memcat, page)

#define luaM_arraysize_(L, n, e) ((cast_to(size_t, (n)) <= SIZE_MAX / (e)) ? (n) * (e) : (luaM_toobig(L), SIZE_MAX))

#define luaM_newarray(L, n, t, memcat) cast_to(t*, luaM_new_(L, luaM_arraysize_(L, n, sizeof(t)), memcat))
#define luaM_trynewarray(L, n, t, memcat) cast_to(t*, luaM_trynew_(L, luaM_arraysize_(L, n, sizeof(t)), memcat))
#define luaM_freearray(L, b, n, t, memcat) luaM_free_(L, (b), (n) * sizeof(t), memcat)
#define luaM_reallocarray(L, v, oldn, n, t, memcat) \
    ((v) = cast_to(t*, luaM_realloc_(L, v, (oldn) * sizeof(t), luaM_arraysize_(L, n, sizeof(t)), memcat)))

LUAI_FUNC void* luaM_new_(lua_State* L, size_t nsize, uint8_t memcat);
LUAI_FUNC void* luaM_trynew_(lua_State* L, size_t nsize, uint8_t memcat);
LUAI_FUNC GCObject* luaM_newgco_(lua_State* L, size_t nsize, uint8_t memcat, int tt, int tag);
LUAI_FUNC GCObject* luaM_newgcofixed_(lua_State* L, size_t nsize, uint8_t memcat, int tt);
LUAI_FUNC GCObject* luaM_newgcocaged_(lua_State* L, size_t nsize, uint8_t memcat, int tt, int tag);
LUAI_FUNC void luaM_free_(lua_State* L, void* block, size_t osize, uint8_t memcat);
LUAI_FUNC void luaM_freegco_(lua_State* L, GCObject* block, size_t osize, uint8_t memcat, lua_Page* page);
LUAI_FUNC void luaM_freegcofixed_(lua_State* L, GCObject* block, size_t osize, uint8_t memcat, lua_Page* page);
LUAI_FUNC void luaM_freegcocaged_(lua_State* L, GCObject* block, size_t osize, uint8_t memcat, lua_Page* page);
LUAI_FUNC void* luaM_realloc_(lua_State* L, void* block, size_t osize, size_t nsize, uint8_t memcat);

LUAI_FUNC l_noret luaM_toobig(lua_State* L);

LUAI_FUNC void luaM_getpagewalkinfo(lua_Page* page, char** start, char** end, int* busyBlocks, int* blockSize);
LUAI_FUNC void luaM_getpageinfo(lua_Page* page, int* pageBlocks, int* busyBlocks, int* blockSize, int* pageSize);
LUAI_FUNC lua_Page* luaM_getnextpage(lua_Page* page);

LUAI_FUNC void luaM_visitpage(lua_Page* page, void* context, bool (*visitor)(void* context, lua_Page* page, GCObject* gco));
LUAI_FUNC void luaM_visitgco(lua_State* L, void* context, bool (*visitor)(void* context, lua_Page* page, GCObject* gco));
