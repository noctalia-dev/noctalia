// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "lstate.h"

#include "ltable.h"
#include "lstring.h"
#include "lfunc.h"
#include "lmem.h"
#include "lgc.h"
#include "ldo.h"
#include "ldebug.h"
#include "ludata.h"

#include <string.h>

LUAU_FASTFLAG(LuauGcTraceUdata)
LUAU_FASTFLAGVARIABLE(LuauEasyStateInit)
LUAU_FASTFLAGVARIABLE(LuauBufferCage)

/*
** Main thread combines a thread state and the global state
*/
typedef struct LG
{
    lua_State l;
    global_State g;
} LG;

static void stack_init(lua_State* L1, lua_State* L)
{
    // initialize CallInfo array
    L1->base_ci = luaM_newarray(L, BASIC_CI_SIZE, CallInfo, L1->memcat);
    L1->ci = L1->base_ci;
    L1->size_ci = BASIC_CI_SIZE;
    L1->end_ci = L1->base_ci + L1->size_ci - 1;
    // initialize stack array
    L1->stack = luaM_newarray(L, BASIC_STACK_SIZE + EXTRA_STACK, TValue, L1->memcat);
    L1->stacksize = BASIC_STACK_SIZE + EXTRA_STACK;
    TValue* stack = L1->stack;
    for (int i = 0; i < BASIC_STACK_SIZE + EXTRA_STACK; i++)
        setnilvalue(stack + i); // erase new stack
    L1->top = stack;
    L1->stack_last = stack + (L1->stacksize - EXTRA_STACK);
    // initialize first ci
    L1->ci->func = L1->top;
    L1->ci->p = nullptr;
    setnilvalue(L1->top++); // `function' entry for this `ci'
    L1->base = L1->ci->base = L1->top;
    L1->ci->top = L1->top + LUA_MINSTACK;
}

static void freestack(lua_State* L, lua_State* L1)
{
    luaM_freearray(L, L1->base_ci, L1->size_ci, CallInfo, L1->memcat);
    luaM_freearray(L, L1->stack, L1->stacksize, TValue, L1->memcat);
}

static LuaTable* weakenvalues(lua_State* L, LuaTable* wt)
{
    LUAU_ASSERT(FFlag::LuauGcTraceUdata);
    LuaTable* mt = luaH_new(L, 0, 1);
    TValue* slot = luaH_setstr(L, mt, L->global->tmname[TM_MODE]);
    setsvalue(L, slot, luaS_newliteral(L, "v"));
    wt->metatable = mt;
    return wt;
}

/*
** open parts that may cause memory-allocation errors
*/
static void f_luaopen(lua_State* L, void* ud)
{
    global_State* g = L->global;
    stack_init(L, L);                             // init stack
    L->gt = luaH_new(L, 0, 2);                    // table of globals
    sethvalue(L, registry(L), luaH_new(L, 0, 2)); // registry
    luaS_resize(L, LUA_MINSTRTABSIZE);            // initial size of string table
    luaT_init(L);
    if (FFlag::LuauGcTraceUdata)
    {
        LuaTable* wt = weakenvalues(L, luaH_new(L, 0, 0)); // weakregistry
        sethvalue(L, &L->global->weakregistry, wt);
    }
    luaS_fix(luaS_newliteral(L, LUA_MEMERRMSG)); // pin to make sure we can always throw this error
    luaS_fix(luaS_newliteral(L, LUA_ERRERRMSG)); // pin to make sure we can always throw this error
    g->GCthreshold = 4 * g->totalbytes;
}

static void preinit_state(lua_State* L, global_State* g)
{
    LUAU_ASSERT(!FFlag::LuauEasyStateInit);

    L->global = g;
    L->stack = NULL;
    L->stacksize = 0;
    L->gt = NULL;
    L->openupval = NULL;
    L->size_ci = 0;
    L->nCcalls = L->baseCcalls = 0;
    L->status = 0;
    L->base_ci = L->ci = NULL;
    L->namecall = NULL;
    L->cachedslot = 0;
    L->singlestep = false;
    L->isactive = false;
    L->activememcat = 0;
    L->finalizers = NULL;
    L->userdata = NULL;
}

static void close_state(lua_State* L)
{
    global_State* g = L->global;
    luaF_close(L, L->stack); // close all upvalues for this thread
    luaC_freeall(L);         // collect all objects
    LUAU_ASSERT(g->strt.nuse == 0);
    luaM_freearray(L, L->global->strt.hash, L->global->strt.size, TString*, 0);
    freestack(L, L);
    for (int i = 0; i < LUA_SIZECLASSES; i++)
    {
        LUAU_ASSERT(g->freepages[i] == NULL);
        LUAU_ASSERT(g->freegcopages[i] == NULL);
        LUAU_ASSERT(g->freegcopages_cage[i] == NULL);
    }
    LUAU_ASSERT(g->allgcopages == NULL);
    LUAU_ASSERT(g->allgcopages_cage == NULL);
    LUAU_ASSERT(g->totalbytes == sizeof(LG));
    LUAU_ASSERT(g->memcatbytes[0] == sizeof(LG));
    for (int i = 1; i < LUA_MEMORY_CATEGORIES; i++)
        LUAU_ASSERT(g->memcatbytes[i] == 0);

    if (L->global->ecb.close)
        L->global->ecb.close(L);

    (*g->frealloc)(g->ud, L, sizeof(LG), 0);
}

lua_State* luaE_newthread(lua_State* L)
{
    lua_State* L1 = luaM_newgco(L, lua_State, sizeof(lua_State), L->activememcat, LUA_TTHREAD);

    if (FFlag::LuauEasyStateInit)
        memset(L1, 0, sizeof(lua_State));

    luaC_init(L, L1, LUA_TTHREAD);

    if (FFlag::LuauEasyStateInit)
        L1->global = L->global;
    else
        preinit_state(L1, L->global);

    L1->activememcat = L->activememcat; // inherit the active memory category
    stack_init(L1, L);                  // init stack
    L1->gt = L->gt;                     // share table of globals
    L1->singlestep = L->singlestep;
    LUAU_ASSERT(iswhite(obj2gco(L1)));
    return L1;
}

void luaE_freethread(lua_State* L, lua_State* L1, lua_Page* page)
{
    global_State* g = L->global;
    if (g->cb.userthread)
        g->cb.userthread(NULL, L1);

    freestack(L, L1);
    luaM_freegco(L, L1, sizeof(lua_State), L1->memcat, page);
}

void lua_resetthread(lua_State* L)
{
    api_check(L, !L->isactive);
    api_check(L, L->status != LUA_OK || L->ci == L->base_ci);

    // close upvalues before clearing anything
    luaF_close(L, L->stack);

    // clear call frames
    CallInfo* ci = L->base_ci;
    ci->p = nullptr;
    ci->func = L->stack;
    ci->base = ci->func + 1;
    ci->top = ci->base + LUA_MINSTACK;
    setnilvalue(ci->func);
    L->ci = ci;
    if (L->size_ci != BASIC_CI_SIZE)
        luaD_reallocCI(L, BASIC_CI_SIZE);
    // clear thread state
    L->status = LUA_OK;
    L->base = L->ci->base;
    L->top = L->ci->base;
    L->nCcalls = L->baseCcalls = 0;
    // clear thread stack
    if (L->stacksize != BASIC_STACK_SIZE + EXTRA_STACK)
        luaD_reallocstack(L, BASIC_STACK_SIZE, 0);
    for (int i = 0; i < L->stacksize; i++)
        setnilvalue(L->stack + i);
    L->finalizers = nullptr;
}

int lua_isthreadreset(lua_State* L)
{
    return L->ci == L->base_ci && L->base == L->top && L->status == LUA_OK;
}

void lua_setmemorycage(lua_State* L, lua_CageAlloc alloc, void* ud)
{
    global_State* g = L->global;
    g->cagealloc = alloc;
    g->cageud = ud;
}

lua_State* lua_newstate(lua_Alloc allocator, void* ud)
{
    if (FFlag::LuauEasyStateInit)
    {
        void* l = allocator(ud, nullptr, 0, sizeof(LG));
        if (l == nullptr)
            return nullptr;

        memset(l, 0, sizeof(LG));

        lua_State* L = &((LG*)l)->l;
        global_State* g = &((LG*)l)->g;

        L->tt = LUA_TTHREAD;
        L->marked = g->currentwhite = bit2mask(WHITE0BIT, FIXEDBIT);

        L->global = g;
        g->mainthread = L;

        g->frealloc = allocator;
        g->ud = ud;

        g->uvhead.u.open.prev = &g->uvhead;
        g->uvhead.u.open.next = &g->uvhead;


        g->ptrenckey[0] = 1;

        g->totalbytes = sizeof(LG);
        g->memcatbytes[0] = sizeof(LG);

        g->gcstate = GCSpause;
        g->gcgoal = LUAI_GCGOAL;
        g->gcstepmul = LUAI_GCSTEPMUL;
        g->gcstepsize = LUAI_GCSTEPSIZE << 10;

        g->cb = lua_Callbacks();
        g->ecb = lua_ExecutionCallbacks();
        g->gcstats = GCStats();

        g->lastprotoid = 1;
        g->lastclassid = 1;

#ifdef LUAI_GCMETRICS
        g->gcmetrics = GCMetrics();
#endif

        if (luaD_rawrunprotected(L, f_luaopen, nullptr) != 0)
        {
            // memory allocation error: free partial state
            close_state(L);
            L = nullptr;
        }
        return L;
    }
    else
    {
        int i;
        lua_State* L;
        global_State* g;
        void* l = (*allocator)(ud, NULL, 0, sizeof(LG));
        if (l == NULL)
            return NULL;
        L = (lua_State*)l;
        g = &((LG*)L)->g;
        L->tt = LUA_TTHREAD;
        L->marked = g->currentwhite = bit2mask(WHITE0BIT, FIXEDBIT);
        L->memcat = 0;
        preinit_state(L, g);
        g->frealloc = allocator;
        g->ud = ud;
        g->mainthread = L;
        g->uvhead.u.open.prev = &g->uvhead;
        g->uvhead.u.open.next = &g->uvhead;
        g->GCthreshold = 0; // mark it as unfinished state
        g->registryfree = 0;
        g->errorjmp = NULL;
        g->rngstate = 0;
        g->ptrenckey[0] = 1;
        g->ptrenckey[1] = 0;
        g->ptrenckey[2] = 0;
        g->ptrenckey[3] = 0;
        for (int i = 0; i < 8; i++)
            g->ptrenckeynew[i] = 0;
        g->ptrencactive = 0;
        g->strt.size = 0;
        g->strt.nuse = 0;
        g->strt.hash = NULL;
        setnilvalue(&g->pseudotemp);
        setnilvalue(registry(L));
        setnilvalue(&g->weakregistry);
        g->weakregistryfree = 0;
        g->weakregistrytop = 0;
        g->embeddergc = NULL;
        g->gcstate = GCSpause;
        g->gray = NULL;
        g->grayagain = NULL;
        g->weak = NULL;
        g->totalbytes = sizeof(LG);
        g->gcgoal = LUAI_GCGOAL;
        g->gcstepmul = LUAI_GCSTEPMUL;
        g->gcstepsize = LUAI_GCSTEPSIZE << 10;

        for (i = 0; i < LUA_SIZECLASSES; i++)
        {
            g->freepages[i] = NULL;
            g->freegcopages[i] = NULL;
            g->freegcopages_cage[i] = NULL;
        }

        g->allpages = NULL;
        g->allgcopages = NULL;
        g->sweepgcopage = NULL;
        g->cagealloc = NULL;
        g->cageud = NULL;
        g->allgcopages_cage = NULL;
        g->sweepgcopage_cage = NULL;

        for (i = 0; i < LUA_T_COUNT; i++)
            g->mt[i] = NULL;

        for (i = 0; i < LUA_UTAG_LIMIT; i++)
        {
            g->udatagc[i] = NULL;
            g->udatamark[i] = NULL;
            g->udatamt[i] = NULL;
        }

        for (i = 0; i < UTAG_INTERNAL_LIMIT; i++)
        {
            lua_UdataDirectAccessData& udatadirect = L->global->udatadirect[i];

            setnilvalue(&udatadirect.indextm);
            setnilvalue(&udatadirect.newindextm);
            setnilvalue(&udatadirect.namecalltm);
            udatadirect.index = NULL;
            udatadirect.newindex = NULL;
            udatadirect.namecall = NULL;
        }

        for (i = 0; i < LUA_LUTAG_LIMIT; i++)
            g->lightuserdataname[i] = NULL;

        for (i = 0; i < UTAG_INTERNAL_LIMIT; i++)
            g->udatadirectfields[i] = NULL;

        for (i = 0; i < LUA_MEMORY_CATEGORIES; i++)
            g->memcatbytes[i] = 0;

        g->memcatbytes[0] = sizeof(LG);

        g->cb = lua_Callbacks();

        g->ecb = lua_ExecutionCallbacks();

        memset(g->ecbdata, 0, LUA_EXECUTION_CALLBACK_STORAGE * sizeof(g->ecbdata[0]));

        g->gcstats = GCStats();
        g->lastprotoid = 1;
        g->lastclassid = 1;

        g->builtinPcall = NULL;
        g->builtinXpcall = NULL;

#ifdef LUAI_GCMETRICS
        g->gcmetrics = GCMetrics();
#endif

        if (luaD_rawrunprotected(L, f_luaopen, NULL) != 0)
        {
            // memory allocation error: free partial state
            close_state(L);
            L = NULL;
        }
        return L;
    }
}

void lua_close(lua_State* L)
{
    L = L->global->mainthread; // only the main thread can be closed
    luaF_close(L, L->stack);   // close all upvalues for this thread
    close_state(L);
}
