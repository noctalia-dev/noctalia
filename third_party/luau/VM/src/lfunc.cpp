// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "lfunc.h"

#include "lstate.h"
#include "lmem.h"
#include "lgc.h"

LUAU_FASTINTVARIABLE(LuauInlineHitsThreshold, 32)

Proto* luaF_newproto(lua_State* L)
{
    Proto* f = luaM_newgco(L, Proto, sizeof(Proto), L->activememcat, LUA_TPROTO);

    luaC_init(L, f, LUA_TPROTO);

    f->nups = 0;
    f->numparams = 0;
    f->is_vararg = 0;
    f->maxstacksize = 0;
    f->flags = 0;

    f->k = NULL;
    f->code = NULL;
    f->p = NULL;
    f->codeentry = NULL;

    f->execdata = NULL;
    f->exectarget = 0;

    f->lineinfo = NULL;
    f->abslineinfo = NULL;
    f->locvars = NULL;
    f->upvalues = NULL;
    f->source = NULL;

    f->debugname = NULL;
    f->debuginsn = NULL;

    f->typeinfo = NULL;

    f->userdata = NULL;

    f->gclist = NULL;

    f->sizecode = 0;
    f->sizep = 0;
    f->sizelocvars = 0;
    f->sizeupvalues = 0;
    f->sizek = 0;
    f->sizelineinfo = 0;
    f->linegaplog2 = 0;
    f->linedefined = 0;
    f->bytecodeid = 0;
    f->sizetypeinfo = 0;

    f->feedbackvec = NULL;
    f->feedbackvecsize = 0;
    f->funid = 0;
    f->optimized = nullptr;
    f->deoptimized = nullptr;
    f->cost = 0;

    return f;
}

Closure* luaF_newLclosure(lua_State* L, int nelems, LuaTable* e, Proto* p)
{
    Closure* c = luaM_newgco(L, Closure, sizeLclosure(nelems), L->activememcat, LUA_TFUNCTION);
    luaC_init(L, c, LUA_TFUNCTION);
    c->isC = 0;
    c->env = e;
    c->nupvalues = cast_byte(nelems);
    c->stacksize = p->maxstacksize;
    c->preload = 0;
    c->l.p = p;
    for (int i = 0; i < nelems; ++i)
        setnilvalue(&c->l.uprefs[i]);
    return c;
}

Closure* luaF_newCclosure(lua_State* L, int nelems, LuaTable* e)
{
    Closure* c = luaM_newgco(L, Closure, sizeCclosure(nelems), L->activememcat, LUA_TFUNCTION);
    luaC_init(L, c, LUA_TFUNCTION);
    c->isC = 1;
    c->env = e;
    c->nupvalues = cast_byte(nelems);
    c->stacksize = LUA_MINSTACK;
    c->preload = 0;
    c->c.f = NULL;
    c->c.cont = NULL;
    c->c.debugname = NULL;

    return c;
}

UpVal* luaF_findupval(lua_State* L, StkId level)
{
    global_State* g = L->global;
    UpVal** pp = &L->openupval;
    UpVal* p;
    while (*pp != NULL && (p = *pp)->v >= level)
    {
        LUAU_ASSERT(!isdead(g, obj2gco(p)));
        LUAU_ASSERT(upisopen(p));
        if (p->v == level)
            return p;

        pp = &p->u.open.threadnext;
    }

    LUAU_ASSERT(L->isactive);
    LUAU_ASSERT(!isblack(obj2gco(L))); // we don't use luaC_threadbarrier because active threads never turn black

    UpVal* uv = luaM_newgco(L, UpVal, sizeof(UpVal), L->activememcat, LUA_TUPVAL); // not found: create a new one
    luaC_init(L, uv, LUA_TUPVAL);
    uv->markedopen = 0;
    uv->v = level; // current value lives in the stack

    // chain the upvalue in the threads open upvalue list at the proper position
    uv->u.open.threadnext = *pp;
    *pp = uv;

    // double link the upvalue in the global open upvalue list
    uv->u.open.prev = &g->uvhead;
    uv->u.open.next = g->uvhead.u.open.next;
    uv->u.open.next->u.open.prev = uv;
    g->uvhead.u.open.next = uv;
    LUAU_ASSERT(uv->u.open.next->u.open.prev == uv && uv->u.open.prev->u.open.next == uv);

    return uv;
}

void luaF_freeupval(lua_State* L, UpVal* uv, lua_Page* page)
{
    luaM_freegco(L, uv, sizeof(UpVal), uv->memcat, page); // free upvalue
}

void luaF_close(lua_State* L, StkId level)
{
    global_State* g = L->global;
    UpVal* uv;
    while (L->openupval != NULL && (uv = L->openupval)->v >= level)
    {
        GCObject* o = obj2gco(uv);
        LUAU_ASSERT(!isblack(o) && upisopen(uv));
        LUAU_ASSERT(!isdead(g, o));

        // unlink value *before* closing it since value storage overlaps
        L->openupval = uv->u.open.threadnext;

        luaF_closeupval(L, uv, /* dead= */ false);
    }
}

void luaF_closeupval(lua_State* L, UpVal* uv, bool dead)
{
    // unlink value from all lists *before* closing it since value storage overlaps
    LUAU_ASSERT(uv->u.open.next->u.open.prev == uv && uv->u.open.prev->u.open.next == uv);
    uv->u.open.next->u.open.prev = uv->u.open.prev;
    uv->u.open.prev->u.open.next = uv->u.open.next;

    if (dead)
        return;

    setobj(L, &uv->u.value, uv->v);
    uv->v = &uv->u.value;
    luaC_upvalclosed(L, uv);
}

void luaF_freeproto(lua_State* L, Proto* f, lua_Page* page)
{
    luaM_freearray(L, f->code, f->sizecode, Instruction, f->memcat);
    luaM_freearray(L, f->p, f->sizep, Proto*, f->memcat);
    luaM_freearray(L, f->k, f->sizek, TValue, f->memcat);
    if (f->lineinfo)
        luaM_freearray(L, f->lineinfo, f->sizelineinfo, uint8_t, f->memcat);
    luaM_freearray(L, f->locvars, f->sizelocvars, struct LocVar, f->memcat);
    luaM_freearray(L, f->upvalues, f->sizeupvalues, TString*, f->memcat);
    if (f->debuginsn)
        luaM_freearray(L, f->debuginsn, f->sizecode, uint8_t, f->memcat);

    if (f->execdata)
        L->global->ecb.destroy(L, f);

    if (f->typeinfo)
        luaM_freearray(L, f->typeinfo, f->sizetypeinfo, uint8_t, f->memcat);

    if (f->feedbackvec)
        luaM_freearray(L, f->feedbackvec, f->feedbackvecsize, FeedbackVectorSlot, f->memcat);

    luaM_freegco(L, f, sizeof(Proto), f->memcat, page);
}

void luaF_freeclosure(lua_State* L, Closure* c, lua_Page* page)
{
    int size = c->isC ? sizeCclosure(c->nupvalues) : sizeLclosure(c->nupvalues);
    luaM_freegco(L, c, size, c->memcat, page);
}

const LocVar* luaF_getlocal(const Proto* f, int local_number, int pc)
{
    for (int i = 0; i < f->sizelocvars; i++)
    {
        if (pc >= f->locvars[i].startpc && pc < f->locvars[i].endpc)
        { // is variable active?
            local_number--;
            if (local_number == 0)
                return &f->locvars[i];
        }
    }

    return NULL; // not found
}

const LocVar* luaF_findlocal(const Proto* f, int local_reg, int pc)
{
    for (int i = 0; i < f->sizelocvars; i++)
        if (local_reg == f->locvars[i].reg && pc >= f->locvars[i].startpc && pc < f->locvars[i].endpc)
            return &f->locvars[i];

    return NULL; // not found
}

bool luaF_recordhit(lua_State* L, Closure* caller, Closure* target, uint32_t slotid)
{
    if (L->global->ecb.inlinefunction == nullptr)
        return false;

    LUAU_ASSERT(!caller->isC);
    Proto* callerp = caller->l.p;
    if (target->isC)
        return false;
    Proto* targetp = target->l.p;
    LUAU_ASSERT(slotid < callerp->feedbackvecsize);
    FeedbackVectorSlot& slot = callerp->feedbackvec[slotid];
    LUAU_ASSERT(slot.kind == FeedbackVectorSlotKind::CALL_TARGET);

    if (slot.call_target.proto == 0)
        slot.call_target.proto = targetp->funid;

    if (slot.call_target.proto != targetp->funid)
        return false;

    slot.call_target.hits++;

    if (static_cast<int>(slot.call_target.hits) >= FInt::LuauInlineHitsThreshold)
    {
        L->global->ecb.inlinefunction(L, caller, target, slot.call_target.pc);
        return false;
    }

    return true;
}
