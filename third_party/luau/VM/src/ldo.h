// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#pragma once

#include "lobject.h"
#include "lstate.h"
#include "luaconf.h"
#include "ldebug.h"

// returns target stack for 'n' extra elements to reallocate
// if possible, stack size growth factor is 2x
#define getgrownstacksize(L, n) ((n) <= L->stacksize ? 2 * L->stacksize : L->stacksize + (n))
#define stacklimitreached(L, n) ((char*)L->stack_last - (char*)L->top <= (n) * (int)sizeof(TValue))

#define luaD_checkstackfornewci(L, n) \
    if (stacklimitreached(L, (n))) \
        luaD_reallocstack(L, getgrownstacksize(L, (n)), 1); \
    else \
        condhardstacktests(luaD_reallocstack(L, L->stacksize - EXTRA_STACK, 1));

#define luaD_checkstack(L, n) \
    if (stacklimitreached(L, (n))) \
        luaD_growstack(L, n); \
    else \
        condhardstacktests(luaD_reallocstack(L, L->stacksize - EXTRA_STACK, 0));

#define incr_top(L) \
    { \
        luaD_checkstack(L, 1); \
        L->top++; \
    }

#define savestack(L, p) check_exp((p) >= L->stack && (p) <= L->stack + L->stacksize, (char*)(p) - (char*)L->stack)
#define restorestack(L, n) check_exp((n) >= 0 && (size_t)(n) <= sizeof(TValue) * L->stacksize, (TValue*)((char*)L->stack + (n)))

#define expandstacklimit(L, p) \
    { \
        LUAU_ASSERT((p) <= (L)->stack_last); \
        if ((L)->ci->top < (p)) \
            (L)->ci->top = (p); \
    }

#define incr_ci(L) ((L->ci == L->end_ci) ? luaD_growCI(L) : (condhardstacktests(luaD_reallocCI(L, L->size_ci)), ++L->ci))

#define saveci(L, p) check_exp((p) >= L->base_ci && (p) <= L->base_ci + L->size_ci, (char*)(p) - (char*)L->base_ci)
#define restoreci(L, n) check_exp((n) >= 0 && (size_t)(n) <= sizeof(CallInfo) * L->size_ci, (CallInfo*)((char*)L->base_ci + (n)))

#define isyielded(L) ((L)->status == LUA_YIELD || (L)->status == LUA_BREAK || (L)->status == SCHEDULED_REENTRY)

// results from luaD_precall
#define PCRLUA 0   // initiated a call to a Lua function
#define PCRC 1     // did a call to a C function
#define PCRYIELD 2 // C function yielded

// return value for a yielded C call
#define C_CALL_YIELD -1

// luaD_call can 'yield' into an immediate reentry
// reentry will remove extra call frames from C call stack and continue execution
// this lua_State::status code is internal and should not be used by users
#define SCHEDULED_REENTRY 0x7f

enum FinallyResult
{
    Finished = 0,
    Error = 1,
    Cancelled = 2,
};

// type of protected functions, to be ran by `runprotected'
typedef void (*Pfunc)(lua_State* L, void* ud);

LUAI_FUNC CallInfo* luaD_growCI(lua_State* L);

LUAI_FUNC bool luaD_performcally(lua_State* L, StkId func, int nresults);
LUAI_FUNC void luaD_callint(lua_State* L, StkId func, int nresults, bool forreentry);
LUAI_FUNC void luaD_call(lua_State* L, StkId func, int nresults);
LUAI_FUNC void luaD_callny(lua_State* L, StkId func, int nresults);
LUAI_FUNC int luaD_pcall(lua_State* L, Pfunc func, void* u, ptrdiff_t oldtop, ptrdiff_t ef);
LUAI_FUNC void luaD_reallocCI(lua_State* L, int newsize);
LUAI_FUNC void luaD_reallocstack(lua_State* L, int newsize, int fornewci);
LUAI_FUNC void luaD_growstack(lua_State* L, int n);
LUAI_FUNC void luaD_checkCstack(lua_State* L);
LUAI_FUNC void luaD_seterrorobj(lua_State* L, int errcode, StkId oldtop);
LUAI_FUNC void luaD_preparefinalizestate(lua_State* L, lua_State* co, FinallyResult finresult);
LUAI_FUNC void luaD_preparefinalize(lua_State* L, lua_State* co, FinallyResult finresult);
LUAI_FUNC int luaD_runfinalizers(lua_State* L, bool returnstatus);

LUAI_FUNC l_noret luaD_throw(lua_State* L, int errcode);
LUAI_FUNC int luaD_rawrunprotected(lua_State* L, Pfunc f, void* ud);
