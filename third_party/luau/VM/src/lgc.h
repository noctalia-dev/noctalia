// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#pragma once

#include "ldo.h"
#include "lobject.h"
#include "lstate.h"

/*
** Default settings for GC tunables (settable via lua_gc)
*/
#define LUAI_GCGOAL 200    // 200% (allow heap to double compared to live heap size)
#define LUAI_GCSTEPMUL 200 // GC runs 'twice the speed' of memory allocation
#define LUAI_GCSTEPSIZE 1  // GC runs every KB of memory allocation

/*
** Possible states of the Garbage Collector
*/
#define GCSpause 0
#define GCSpropagate 1
#define GCSpropagateagain 2
#define GCSatomic 3
#define GCSsweep 4

/*
** The main invariant of the garbage collector, while marking objects,
** is that a black object can never point to a white one. This invariant
** is not being enforced during a sweep phase, and is restored when sweep
** ends.
*/
#define keepinvariant(g) ((g)->gcstate == GCSpropagate || (g)->gcstate == GCSpropagateagain || (g)->gcstate == GCSatomic)

/*
** some useful bit tricks
*/
#define resetbits(x, m) ((x) &= cast_to(uint8_t, ~(m)))
#define setbits(x, m) ((x) |= (m))
#define testbits(x, m) ((x) & (m))
#define bitmask(b) (1 << (b))
#define bit2mask(b1, b2) (bitmask(b1) | bitmask(b2))
#define l_setbit(x, b) setbits(x, bitmask(b))
#define resetbit(x, b) resetbits(x, bitmask(b))
#define testbit(x, b) testbits(x, bitmask(b))
#define set2bits(x, b1, b2) setbits(x, (bit2mask(b1, b2)))
#define reset2bits(x, b1, b2) resetbits(x, (bit2mask(b1, b2)))
#define test2bits(x, b1, b2) testbits(x, (bit2mask(b1, b2)))

/*
** Layout for bit use in `marked' field:
** bit 0 - object is white (type 0)
** bit 1 - object is white (type 1)
** bit 2 - object is black
** bit 3 - object is fixed (should not be collected)
*/

#define WHITE0BIT 0
#define WHITE1BIT 1
#define BLACKBIT 2
#define FIXEDBIT 3
#define WHITEBITS bit2mask(WHITE0BIT, WHITE1BIT)

#define iswhite(x) test2bits((x)->gch.marked, WHITE0BIT, WHITE1BIT)
#define isblack(x) testbit((x)->gch.marked, BLACKBIT)
#define isgray(x) (!testbits((x)->gch.marked, WHITEBITS | bitmask(BLACKBIT)))
#define isfixed(x) testbit((x)->gch.marked, FIXEDBIT)

#define otherwhite(g) (g->currentwhite ^ WHITEBITS)
#define isdead(g, v) (((v)->gch.marked & (WHITEBITS | bitmask(FIXEDBIT))) == (otherwhite(g) & WHITEBITS))

#define changewhite(x) ((x)->gch.marked ^= WHITEBITS)
#define gray2black(x) l_setbit((x)->gch.marked, BLACKBIT)

#define luaC_white(g) cast_to(uint8_t, ((g)->currentwhite) & WHITEBITS)

#define luaC_needsGC(L) (L->global->totalbytes >= L->global->GCthreshold)

#define luaC_checkGC(L) \
    { \
        condhardstacktests(luaD_reallocstack(L, L->stacksize - EXTRA_STACK, 0)); \
        if (luaC_needsGC(L)) \
        { \
            condhardmemtests(luaC_validate(L), 1); \
            luaC_step(L, true); \
        } \
        else \
        { \
            condhardmemtests(luaC_validate(L), 2); \
        } \
    }

#define luaC_barrier(L, p, v) \
    { \
        if (iscollectable(v) && isblack(obj2gco(p)) && iswhite(gcvalue(v))) \
            luaC_barrierf(L, obj2gco(p), gcvalue(v)); \
    }

#define luaC_barriert(L, t, v) \
    { \
        if (iscollectable(v) && isblack(obj2gco(t)) && iswhite(gcvalue(v))) \
            luaC_barriertable(L, t, gcvalue(v)); \
    }

#define luaC_barrierfast(L, t) \
    { \
        if (isblack(obj2gco(t))) \
            luaC_barrierback(L, obj2gco(t), &t->gclist); \
    }

#define luaC_objbarrier(L, p, o) \
    { \
        if (isblack(obj2gco(p)) && iswhite(obj2gco(o))) \
            luaC_barrierf(L, obj2gco(p), obj2gco(o)); \
    }

#define luaC_threadbarrier(L) \
    { \
        if (isblack(obj2gco(L))) \
            luaC_barrierback(L, obj2gco(L), &L->gclist); \
    }

#define luaC_objectbarrier(L) \
    { \
        if (isblack(obj2gco(L))) \
            luaC_barrierback(L, obj2gco(L), &L->gclist); \
    }

#define luaC_init(L, o, tt_) \
    { \
        o->marked = luaC_white(L->global); \
        o->tt = tt_; \
        o->memcat = L->activememcat; \
    }

LUAI_FUNC void luaC_freeall(lua_State* L);
LUAI_FUNC size_t luaC_step(lua_State* L, bool assist);
LUAI_FUNC void luaC_fullgc(lua_State* L);
LUAI_FUNC void luaC_upvalclosed(lua_State* L, UpVal* uv);
LUAI_FUNC void luaC_barrierf(lua_State* L, GCObject* o, GCObject* v);
LUAI_FUNC void luaC_barriertable(lua_State* L, LuaTable* t, GCObject* v);
LUAI_FUNC void luaC_barrierback(lua_State* L, GCObject* o, GCObject** gclist);
LUAI_FUNC void luaC_validate(lua_State* L);
LUAI_FUNC void luaC_dump(lua_State* L, void* file, const char* (*categoryName)(lua_State* L, uint8_t memcat));
LUAI_FUNC void luaC_enumheap(
    lua_State* L,
    void* context,
    void (*node)(void* context, void* ptr, uint8_t tt, uint8_t memcat, size_t size, const char* name),
    void (*edge)(void* context, void* from, void* to, const char* name)
);
LUAI_FUNC int64_t luaC_allocationrate(lua_State* L);
LUAI_FUNC const char* luaC_statename(int state);
