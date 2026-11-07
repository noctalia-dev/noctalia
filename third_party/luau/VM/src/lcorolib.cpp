// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "lualib.h"

#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lstate.h"
#include "lvm.h"

LUAU_FASTFLAG(LuauCoroutineFinally)

// TODO: Remove with FFlagLuauCoroutineFinally
#define CO_STATUS_ERROR -1
#define CO_STATUS_BREAK -2

static const char* const statnames[] = {"running", "suspended", "normal", "dead", "dead"}; // dead appears twice for LUA_COERR and LUA_COFIN

static int costatus(lua_State* L)
{
    lua_State* co = lua_tothread(L, 1);
    luaL_argexpected(L, co, 1, "thread");
    lua_pushstring(L, statnames[lua_costatus(L, co)]);
    return 1;
}

static int interruptThread(lua_State* L, lua_State* co)
{
    // notify the debugger that the thread was suspended
    if (L->global->cb.debuginterrupt)
        luau_callhook(L, L->global->cb.debuginterrupt, co);

    return lua_break(L);
}

template<bool Wrap>
static int auxresumefinisherror(lua_State* L, lua_State* co)
{
    if (Wrap)
    {
        if (lua_isstring(L, -1))
        {                     // error object is a string?
            luaL_where(L, 1); // add extra info
            lua_insert(L, -2);
            lua_concat(L, 2);
        }
        lua_error(L); // propagate error
    }
    else
    {
        lua_pushboolean(L, 0);
        lua_insert(L, -2);
        return 2; // return false + error message
    }
}

template<bool Wrap>
static LUAU_FORCEINLINE int auxresumefinishsuccess(lua_State* L, lua_State* co)
{
    int nres = cast_int(co->top - co->base);

    if (nres + 1 > LUA_MINSTACK && !lua_checkstack(L, nres + 1))
        luaL_error(L, "too many results to resume");

    if (!Wrap)
        lua_pushboolean(L, 1);

    if (nres != 0)
    {
        // move yielded values
        luaC_threadbarrier(L);

        StkId totop = L->top;
        StkId fromtop = co->top - nres;

        for (int i = 0; i < nres; i++)
            setobj2s(L, totop + i, fromtop + i);

        co->top = fromtop;
        L->top = totop + nres;
    }

    return nres + (Wrap ? 0 : 1); // non-wrap version returns additional status
}

template<bool Wrap>
static LUAU_FORCEINLINE int auxresumefinish(lua_State* L, lua_State* co, int status)
{
    LUAU_ASSERT(FFlag::LuauCoroutineFinally);

    if (status == LUA_YIELD)
        return auxresumefinishsuccess<Wrap>(L, co);

    // we might have reached another breakpoint in the target coroutine
    if (LUAU_UNLIKELY(status == LUA_BREAK))
        return interruptThread(L, co);

    // complete finalizers, but only if it wasn't a failure of the resume itself
    if (status == co->status && co->finalizers)
    {
        if (Wrap)
            luaL_error(L, "cannot run finalizers from a wrapped coroutine");

        luaD_preparefinalize(L, co, co->status == LUA_OK ? FinallyResult::Finished : FinallyResult::Error);
        return luaD_runfinalizers(L, /* returnstatus */ true);
    }

    if (status == LUA_OK)
        return auxresumefinishsuccess<Wrap>(L, co);

    lua_xmove(co, L, 1);
    return auxresumefinisherror<Wrap>(L, co);
}

template<bool Wrap>
static int auxresume(lua_State* L, lua_State* co, int narg)
{
    // error handling for edge cases
    if (co->status != LUA_YIELD)
    {
        int status = lua_costatus(L, co);
        if (status != LUA_COSUS)
        {
            lua_pushfstring(L, "cannot resume %s coroutine", statnames[status]);

            if (FFlag::LuauCoroutineFinally)
                return auxresumefinisherror<Wrap>(L, co);
            else
                return CO_STATUS_ERROR;
        }
    }

    if (narg)
    {
        if (!lua_checkstack(co, narg))
            luaL_error(L, "too many arguments to resume");
        lua_xmove(L, co, narg);
    }
    else
    {
        // coroutine might be completely full already
        if ((co->top - co->base) > LUAI_MAXCSTACK)
            luaL_error(L, "too many arguments to resume");
    }

    co->singlestep = L->singlestep;

    int status = lua_resume(co, L, narg);

    if (FFlag::LuauCoroutineFinally)
    {
        return auxresumefinish<Wrap>(L, co, status);
    }
    else
    {
        if (status == 0 || status == LUA_YIELD)
        {
            int nres = cast_int(co->top - co->base);
            if (nres)
            {
                // +1 accounts for true/false status in resumefinish
                if (nres + 1 > LUA_MINSTACK && !lua_checkstack(L, nres + 1))
                    luaL_error(L, "too many results to resume");
                lua_xmove(co, L, nres); // move yielded values
            }
            return nres;
        }
        else if (status == LUA_BREAK)
        {
            return CO_STATUS_BREAK;
        }
        else
        {
            lua_xmove(co, L, 1); // move error message
            return CO_STATUS_ERROR;
        }
    }
}

static int auxresumecont(lua_State* L, lua_State* co)
{
    LUAU_ASSERT(!FFlag::LuauCoroutineFinally);

    if (co->status == 0 || co->status == LUA_YIELD)
    {
        int nres = cast_int(co->top - co->base);
        if (!lua_checkstack(L, nres + 1))
            luaL_error(L, "too many results to resume");
        lua_xmove(co, L, nres); // move yielded values
        return nres;
    }
    else
    {
        lua_rawcheckstack(L, 2);
        lua_xmove(co, L, 1); // move error message
        return CO_STATUS_ERROR;
    }
}

static int coresumefinish(lua_State* L, int r)
{
    LUAU_ASSERT(!FFlag::LuauCoroutineFinally);

    if (r < 0)
    {
        lua_pushboolean(L, 0);
        lua_insert(L, -2);
        return 2; // return false + error message
    }
    else
    {
        lua_pushboolean(L, 1);
        lua_insert(L, -(r + 1));
        return r + 1; // return true + `resume' returns
    }
}

static int coresumey(lua_State* L)
{
    lua_State* co = lua_tothread(L, 1);
    luaL_argexpected(L, co, 1, "thread");
    int narg = cast_int(L->top - L->base) - 1;

    if (FFlag::LuauCoroutineFinally)
    {
        return auxresume<false>(L, co, narg);
    }
    else
    {
        int r = auxresume<false>(L, co, narg);

        if (r == CO_STATUS_BREAK)
            return interruptThread(L, co);

        return coresumefinish(L, r);
    }
}

static int coresumecont(lua_State* L, int status)
{
    if (FFlag::LuauCoroutineFinally)
    {
        // finalizer errored, return false status and the error
        if (status != LUA_OK)
        {
            lua_pushboolean(L, 0);
            lua_insert(L, -2);
            return 2;
        }

        lua_State* co = lua_tothread(L, 1);
        luaL_argexpected(L, co, 1, "thread");

        // if we haven't completed auxresume yet
        if (lua_gettop(L) == 1)
            return auxresumefinish<false>(L, co, co->status);

        return luaD_runfinalizers(L, /* returnstatus */ true);
    }
    else
    {
        lua_State* co = lua_tothread(L, 1);
        luaL_argexpected(L, co, 1, "thread");

        // if coroutine still hasn't yielded after the break, break current thread again
        if (co->status == LUA_BREAK)
            return interruptThread(L, co);

        int r = auxresumecont(L, co);

        return coresumefinish(L, r);
    }
}

static int auxwrapfinish(lua_State* L, int r)
{
    LUAU_ASSERT(!FFlag::LuauCoroutineFinally);

    if (r < 0)
    {
        if (lua_isstring(L, -1))
        {                     // error object is a string?
            luaL_where(L, 1); // add extra info
            lua_insert(L, -2);
            lua_concat(L, 2);
        }
        lua_error(L); // propagate error
    }
    return r;
}

static int auxwrapy(lua_State* L)
{
    lua_State* co = lua_tothread(L, lua_upvalueindex(1));
    int narg = cast_int(L->top - L->base);

    if (FFlag::LuauCoroutineFinally)
    {
        return auxresume<true>(L, co, narg);
    }
    else
    {
        int r = auxresume<true>(L, co, narg);

        if (r == CO_STATUS_BREAK)
            return interruptThread(L, co);

        return auxwrapfinish(L, r);
    }
}

static int auxwrapcont(lua_State* L, int status)
{
    lua_State* co = lua_tothread(L, lua_upvalueindex(1));

    if (FFlag::LuauCoroutineFinally)
    {
        return auxresumefinish<true>(L, co, co->status);
    }
    else
    {
        // we might have reached another breakpoint in the target coroutine
        if (co->status == LUA_BREAK)
            return interruptThread(L, co);

        int r = auxresumecont(L, co);

        return auxwrapfinish(L, r);
    }
}

static int cocreate(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_State* NL = lua_newthread(L);
    lua_xpush(L, NL, 1); // push function on top of NL
    return 1;
}

static int cowrap(lua_State* L)
{
    cocreate(L);

    lua_pushcclosurek(L, auxwrapy, NULL, 1, auxwrapcont);
    return 1;
}

static int coyield(lua_State* L)
{
    int nres = cast_int(L->top - L->base);
    return lua_yield(L, nres);
}

static int corunning(lua_State* L)
{
    if (lua_pushthread(L))
        lua_pushnil(L); // main thread is not a coroutine
    return 1;
}

static int coyieldable(lua_State* L)
{
    lua_pushboolean(L, lua_isyieldable(L));
    return 1;
}

static int coclose(lua_State* L)
{
    LUAU_ASSERT(!FFlag::LuauCoroutineFinally);

    lua_State* co = lua_tothread(L, 1);
    luaL_argexpected(L, co, 1, "thread");

    int status = lua_costatus(L, co);
    if (status != LUA_COFIN && status != LUA_COERR && status != LUA_COSUS)
        luaL_error(L, "cannot close %s coroutine", statnames[status]);

    if (co->status == LUA_OK || co->status == LUA_YIELD)
    {
        lua_pushboolean(L, true);
        lua_resetthread(co);
        return 1;
    }
    else
    {
        lua_pushboolean(L, false);

        if (co->status == LUA_ERRMEM)
            lua_pushstring(L, LUA_MEMERRMSG);
        else if (co->status == LUA_ERRERR)
            lua_pushstring(L, LUA_ERRERRMSG);
        else if (lua_gettop(co))
            lua_xmove(co, L, 1); // move error message

        lua_resetthread(co);
        return 2;
    }
}

static int coclosey(lua_State* L)
{
    LUAU_ASSERT(FFlag::LuauCoroutineFinally);

    lua_State* co = lua_tothread(L, 1);
    luaL_argexpected(L, co, 1, "thread");

    int status = lua_costatus(L, co);
    if (status != LUA_COFIN && status != LUA_COERR && status != LUA_COSUS)
        luaL_error(L, "cannot close %s coroutine", statnames[status]);

    if (co->status == LUA_OK || co->status == LUA_YIELD)
    {
        if (co->finalizers)
        {
            lua_settop(L, 1);
            luaD_preparefinalizestate(L, co, FinallyResult::Cancelled);
            lua_resetthread(co); // can be reused immediately
            return luaD_runfinalizers(L, /* returnstatus */ true);
        }

        lua_pushboolean(L, true);
        lua_resetthread(co);
        return 1;
    }
    else
    {
        LUAU_ASSERT(!co->finalizers); // finalizers should have already been executed

        lua_pushboolean(L, false);

        if (co->status == LUA_ERRMEM)
            lua_pushstring(L, LUA_MEMERRMSG);
        else if (co->status == LUA_ERRERR)
            lua_pushstring(L, LUA_ERRERRMSG);
        else if (lua_gettop(co))
            lua_xmove(co, L, 1); // move error message

        lua_resetthread(co);
        return 2;
    }
}

static int coclosecont(lua_State* L, int status)
{
    LUAU_ASSERT(FFlag::LuauCoroutineFinally);

    if (status != LUA_OK)
    {
        lua_pushboolean(L, 0);
        lua_insert(L, -2);
        return 2;
    }

    return luaD_runfinalizers(L, /* returnstatus */ true);
}

// coroutine.finally(co, callback)
static int cofinally(lua_State* L)
{
    lua_State* co = lua_tothread(L, 1);
    luaL_argexpected(L, co, 1, "thread");

    // callback can be any callable object
    if (lua_isnoneornil(L, 2))
        luaL_error(L, "missing argument #%d", 2);

    lua_addfinalizer(L, co, 2);
    return 0;
}

static const luaL_Reg co_funcs_DEPRECATED[] = {
    {"create", cocreate},
    {"running", corunning},
    {"status", costatus},
    {"wrap", cowrap},
    {"yield", coyield},
    {"isyieldable", coyieldable},
    {"close", coclose},
    {NULL, NULL},
};

static const luaL_Reg co_funcs[] = {
    {"create", cocreate},
    {"running", corunning},
    {"status", costatus},
    {"wrap", cowrap},
    {"yield", coyield},
    {"isyieldable", coyieldable},
    {"finally", cofinally},
    {NULL, NULL},
};

int luaopen_coroutine(lua_State* L)
{
    if (FFlag::LuauCoroutineFinally)
        luaL_register(L, LUA_COLIBNAME, co_funcs);
    else
        luaL_register(L, LUA_COLIBNAME, co_funcs_DEPRECATED);

    lua_pushcclosurek(L, coresumey, "resume", 0, coresumecont);
    lua_setfield(L, -2, "resume");

    if (FFlag::LuauCoroutineFinally)
    {
        lua_pushcclosurek(L, coclosey, "close", 0, coclosecont);
        lua_setfield(L, -2, "close");
    }

    return 1;
}
