// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "lgc.h"

#include "lfunc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ludata.h"
#include "lbuffer.h"

#include <string.h>
#include <stdio.h>

LUAU_FASTFLAGVARIABLE(LuauEnumMoreEdges)
LUAU_FASTFLAG(LuauFrozenMetaButterfly)

static void validateobjref(global_State* g, GCObject* f, GCObject* t)
{
    LUAU_ASSERT(!isdead(g, t));

    if (keepinvariant(g))
    {
        // basic incremental invariant: black can't point to white
        LUAU_ASSERT(!(isblack(f) && iswhite(t)));
    }
}

static void validateref(global_State* g, GCObject* f, TValue* v)
{
    if (iscollectable(v))
    {
        LUAU_ASSERT(ttype(v) == gcvalue(v)->gch.tt);
        validateobjref(g, f, gcvalue(v));
    }
}

static void validatetable(global_State* g, LuaTable* h)
{
    int sizenode = 1 << h->lsizenode;

    LUAU_ASSERT(h->lastfree <= sizenode);

    if (h->metatable)
        validateobjref(g, obj2gco(h), obj2gco(h->metatable));

    for (int i = 0; i < h->sizearray; ++i)
        validateref(g, obj2gco(h), &h->array[i]);

    for (int i = 0; i < sizenode; ++i)
    {
        LuaNode* n = &h->node[i];

        LUAU_ASSERT(ttype(gkey(n)) != LUA_TDEADKEY || ttisnil(gval(n)));
        LUAU_ASSERT(i + gnext(n) >= 0 && i + gnext(n) < sizenode);

        if (!ttisnil(gval(n)))
        {
            TValue k = {};
            k.tt = gkey(n)->tt;
            k.value = gkey(n)->value;

            validateref(g, obj2gco(h), &k);
            validateref(g, obj2gco(h), gval(n));
        }
    }
}

static void validateclosure(global_State* g, Closure* cl)
{
    validateobjref(g, obj2gco(cl), obj2gco(cl->env));

    if (cl->isC)
    {
        for (int i = 0; i < cl->nupvalues; ++i)
            validateref(g, obj2gco(cl), &cl->c.upvals[i]);
    }
    else
    {
        LUAU_ASSERT(cl->nupvalues == cl->l.p->nups);

        validateobjref(g, obj2gco(cl), obj2gco(cl->l.p));

        for (int i = 0; i < cl->nupvalues; ++i)
            validateref(g, obj2gco(cl), &cl->l.uprefs[i]);
    }
}

static void validatestack(global_State* g, lua_State* l)
{
    validateobjref(g, obj2gco(l), obj2gco(l->gt));

    for (CallInfo* ci = l->base_ci; ci <= l->ci; ++ci)
    {
        LUAU_ASSERT(l->stack <= ci->base);
        LUAU_ASSERT(ci->func <= ci->base && ci->base <= ci->top);
        LUAU_ASSERT(ci->top <= l->stack_last);
    }

    // note: stack refs can violate gc invariant so we only check for liveness
    for (StkId o = l->stack; o < l->top; ++o)
        checkliveness(g, o);

    if (l->namecall)
        validateobjref(g, obj2gco(l), obj2gco(l->namecall));

    if (l->finalizers)
        validateobjref(g, obj2gco(l), obj2gco(l->finalizers));

    for (UpVal* uv = l->openupval; uv; uv = uv->u.open.threadnext)
    {
        LUAU_ASSERT(uv->tt == LUA_TUPVAL);
        LUAU_ASSERT(upisopen(uv));
        LUAU_ASSERT(uv->u.open.next->u.open.prev == uv && uv->u.open.prev->u.open.next == uv);
        LUAU_ASSERT(!isblack(obj2gco(uv))); // open upvalues are never black
    }
}

static void validateproto(global_State* g, Proto* f)
{
    if (f->source)
        validateobjref(g, obj2gco(f), obj2gco(f->source));

    if (f->debugname)
        validateobjref(g, obj2gco(f), obj2gco(f->debugname));

    for (int i = 0; i < f->sizek; ++i)
        validateref(g, obj2gco(f), &f->k[i]);

    for (int i = 0; i < f->sizeupvalues; ++i)
        if (f->upvalues[i])
            validateobjref(g, obj2gco(f), obj2gco(f->upvalues[i]));

    for (int i = 0; i < f->sizep; ++i)
        if (f->p[i])
            validateobjref(g, obj2gco(f), obj2gco(f->p[i]));

    for (int i = 0; i < f->sizelocvars; i++)
        if (f->locvars[i].varname)
            validateobjref(g, obj2gco(f), obj2gco(f->locvars[i].varname));
}

static void validateclass(global_State* g, LuauClass* lco)
{
    GCObject* obj = obj2gco(lco);
    validateobjref(g, obj, obj2gco(lco->name));
    if (lco->super)
        validateobjref(g, obj, obj2gco(lco->super));
    validateobjref(g, obj, obj2gco(lco->memberstooffset));
    for (uint32_t i = 0; i < lco->numberofallmembers; i++)
    {
        validateobjref(g, obj, obj2gco(lco->offsettomember[i]));
        if (i >= lco->numberofinstancemembers)
            validateref(g, obj, &lco->staticmembers[i - lco->numberofinstancemembers]);
    }
    if (lco->metatable)
        validateobjref(g, obj, obj2gco(lco->metatable));
    if (lco->instancemetatable)
        validateobjref(g, obj, obj2gco(lco->instancemetatable));
}

static void validateobject(global_State* g, LuauObject* inst)
{
    GCObject* obj = obj2gco(inst);
    validateobjref(g, obj, obj2gco(inst->lclass));
    for (uint32_t i = 0; i < inst->numberofmembers; i++)
        validateref(g, obj, &inst->members[i]);
}

static void validateobj(global_State* g, GCObject* o)
{
    // dead objects can only occur during sweep
    if (isdead(g, o))
    {
        LUAU_ASSERT(g->gcstate == GCSsweep);
        return;
    }

    switch (o->gch.tt)
    {
    case LUA_TSTRING:
        break;

    case LUA_TTABLE:
        validatetable(g, gco2h(o));
        break;

    case LUA_TFUNCTION:
        validateclosure(g, gco2cl(o));
        break;

    case LUA_TUSERDATA:
        if (gco2u(o)->metatable)
            validateobjref(g, o, obj2gco(gco2u(o)->metatable));
        break;

    case LUA_TTHREAD:
        validatestack(g, gco2th(o));
        break;

    case LUA_TVECTOR:
        break;

    case LUA_TBUFFER:
        break;

    case LUA_TPROTO:
        validateproto(g, gco2p(o));
        break;

    case LUA_TUPVAL:
        validateref(g, o, gco2uv(o)->v);
        break;

    case LUA_TCLASS:
        validateclass(g, gco2class(o));
        break;

    case LUA_TOBJECT:
        validateobject(g, gco2object(o));
        break;

    default:
        LUAU_ASSERT(!"unexpected object type");
    }
}

static void validategraylist(global_State* g, GCObject* o)
{
    if (!keepinvariant(g))
        return;

    while (o)
    {
        LUAU_ASSERT(isgray(o));

        switch (o->gch.tt)
        {
        case LUA_TTABLE:
            o = gco2h(o)->gclist;
            break;
        case LUA_TFUNCTION:
            o = gco2cl(o)->gclist;
            break;
        case LUA_TTHREAD:
            o = gco2th(o)->gclist;
            break;
        case LUA_TCLASS:
            o = gco2class(o)->gclist;
            break;
        case LUA_TOBJECT:
            o = gco2object(o)->gclist;
            break;
        case LUA_TPROTO:
            o = gco2p(o)->gclist;
            break;
        default:
            LUAU_ASSERT(!"unknown object in gray list");
            return;
        }
    }
}

static bool validategco(void* context, lua_Page* page, GCObject* gco)
{
    lua_State* L = (lua_State*)context;
    global_State* g = L->global;

    validateobj(g, gco);
    return false;
}

void luaC_validate(lua_State* L)
{
    global_State* g = L->global;

    LUAU_ASSERT(!isdead(g, obj2gco(g->mainthread)));
    checkliveness(g, &g->registry);

    for (int i = 0; i < LUA_T_COUNT; ++i)
    {
        if (g->mt[i])
            LUAU_ASSERT(!isdead(g, obj2gco(g->mt[i])));
    }

    for (int i = 0; i < LUA_UTAG_LIMIT; i++)
    {
        if (g->udatamt[i])
            LUAU_ASSERT(!isdead(g, obj2gco(g->udatamt[i])));
    }

    for (int i = 0; i < UTAG_INTERNAL_LIMIT; i++)
    {
        checkliveness(g, &g->udatadirect[i].indextm);
        checkliveness(g, &g->udatadirect[i].newindextm);
        checkliveness(g, &g->udatadirect[i].namecalltm);

        if (g->udatadirectfields[i])
            LUAU_ASSERT(!isdead(g, obj2gco(g->udatadirectfields[i])));
    }

    validategraylist(g, g->weak);
    validategraylist(g, g->gray);
    validategraylist(g, g->grayagain);

    validategco(L, NULL, obj2gco(g->mainthread));

    luaM_visitgco(L, L, validategco);

    for (UpVal* uv = g->uvhead.u.open.next; uv != &g->uvhead; uv = uv->u.open.next)
    {
        LUAU_ASSERT(uv->tt == LUA_TUPVAL);
        LUAU_ASSERT(upisopen(uv));
        LUAU_ASSERT(uv->u.open.next->u.open.prev == uv && uv->u.open.prev->u.open.next == uv);
        LUAU_ASSERT(!isblack(obj2gco(uv))); // open upvalues are never black
    }
}

inline bool safejson(char ch)
{
    return unsigned(ch) < 128 && ch >= 32 && ch != '\\' && ch != '\"';
}

static void dumpref(FILE* f, GCObject* o)
{
    fprintf(f, "\"%p\"", o);
}

static void dumprefs(FILE* f, TValue* data, size_t size)
{
    bool first = true;

    for (size_t i = 0; i < size; ++i)
    {
        if (iscollectable(&data[i]))
        {
            if (!first)
                fputc(',', f);
            first = false;

            dumpref(f, gcvalue(&data[i]));
        }
    }
}

static void dumpstringdata(FILE* f, const char* data, size_t len)
{
    for (size_t i = 0; i < len; ++i)
        fputc(safejson(data[i]) ? data[i] : '?', f);
}

static void dumpstring(FILE* f, TString* ts)
{
    fprintf(f, "{\"type\":\"string\",\"cat\":%d,\"size\":%d,\"data\":\"", ts->memcat, int(sizestring(ts->len)));
    dumpstringdata(f, ts->data, ts->len);
    fprintf(f, "\"}");
}

static void dumptable(FILE* f, LuaTable* h)
{
    size_t size = sizeof(LuaTable) + (h->node == &luaH_dummynode ? 0 : sizenode(h) * sizeof(LuaNode)) + h->sizearray * sizeof(TValue);

    if (FFlag::LuauFrozenMetaButterfly && hasmetacache(h))
        size += TM_N * sizeof(TValue);

    fprintf(f, "{\"type\":\"table\",\"cat\":%d,\"size\":%d", h->memcat, int(size));

    if (h->node != &luaH_dummynode)
    {
        fprintf(f, ",\"pairs\":[");

        bool first = true;

        for (int i = 0; i < sizenode(h); ++i)
        {
            const LuaNode& n = h->node[i];

            if (!ttisnil(&n.val) && (iscollectable(&n.key) || iscollectable(&n.val)))
            {
                if (!first)
                    fputc(',', f);
                first = false;

                if (iscollectable(&n.key))
                    dumpref(f, gcvalue(&n.key));
                else
                    fprintf(f, "null");

                fputc(',', f);

                if (iscollectable(&n.val))
                    dumpref(f, gcvalue(&n.val));
                else
                    fprintf(f, "null");
            }
        }

        fprintf(f, "]");
    }
    if (h->sizearray)
    {
        fprintf(f, ",\"array\":[");
        dumprefs(f, h->array, h->sizearray);
        fprintf(f, "]");
    }
    if (h->metatable)
    {
        fprintf(f, ",\"metatable\":");
        dumpref(f, obj2gco(h->metatable));
    }
    fprintf(f, "}");
}

static void dumpclosure(FILE* f, Closure* cl)
{
    fprintf(
        f, "{\"type\":\"function\",\"cat\":%d,\"size\":%d", cl->memcat, cl->isC ? int(sizeCclosure(cl->nupvalues)) : int(sizeLclosure(cl->nupvalues))
    );

    fprintf(f, ",\"env\":");
    dumpref(f, obj2gco(cl->env));

    if (cl->isC)
    {
        if (TString* str = cl->c.debugname)
            fprintf(f, ",\"name\":\"%s\"", getstr(str));

        if (cl->nupvalues)
        {
            fprintf(f, ",\"upvalues\":[");
            dumprefs(f, cl->c.upvals, cl->nupvalues);
            fprintf(f, "]");
        }
    }
    else
    {
        if (cl->l.p->debugname)
            fprintf(f, ",\"name\":\"%s\"", getstr(cl->l.p->debugname));

        fprintf(f, ",\"proto\":");
        dumpref(f, obj2gco(cl->l.p));
        if (cl->nupvalues)
        {
            fprintf(f, ",\"upvalues\":[");
            dumprefs(f, cl->l.uprefs, cl->nupvalues);
            fprintf(f, "]");
        }
    }
    fprintf(f, "}");
}

static void dumpudata(FILE* f, Udata* u)
{
    fprintf(f, "{\"type\":\"userdata\",\"cat\":%d,\"size\":%d,\"tag\":%d", u->memcat, int(sizeudata(u->len)), u->tag);

    if (u->metatable)
    {
        fprintf(f, ",\"metatable\":");
        dumpref(f, obj2gco(u->metatable));
    }
    fprintf(f, "}");
}

static void dumpthread(FILE* f, lua_State* th)
{
    size_t size = sizeof(lua_State) + sizeof(TValue) * th->stacksize + sizeof(CallInfo) * th->size_ci;

    fprintf(f, "{\"type\":\"thread\",\"cat\":%d,\"size\":%d", th->memcat, int(size));

    fprintf(f, ",\"env\":");
    dumpref(f, obj2gco(th->gt));

    if (th->finalizers)
    {
        fprintf(f, ",\"finalizers\":");
        dumpref(f, obj2gco(th->finalizers));
    }

    Proto* cip = nullptr;
    for (CallInfo* ci = th->base_ci; ci <= th->ci; ++ci)
    {
        if (ttisfunction(ci->func))
        {
            cip = ci->p;
            break;
        }
    }

    if (cip != nullptr && cip->source)
    {
        fprintf(f, ",\"source\":\"");
        dumpstringdata(f, cip->source->data, cip->source->len);
        fprintf(f, "\",\"line\":%d", cip->linedefined);
    }

    if (th->top > th->stack)
    {
        fprintf(f, ",\"stack\":[");
        dumprefs(f, th->stack, th->top - th->stack);
        fprintf(f, "]");

        CallInfo* ci = th->base_ci;
        bool first = true;

        fprintf(f, ",\"stacknames\":[");
        for (StkId v = th->stack; v < th->top; ++v)
        {
            if (!iscollectable(v))
                continue;

            while (ci < th->ci && v >= (ci + 1)->func)
                ci++;

            if (!first)
                fputc(',', f);
            first = false;

            if (v == ci->func)
            {
                Closure* cl = ci_func(ci);

                if (cl->isC)
                {
                    fprintf(f, "\"frame:%s\"", cl->c.debugname ? getstr(cl->c.debugname) : "[C]");
                }
                else
                {
                    Proto* p = ci->p;
                    fprintf(f, "\"frame:");
                    if (p->source)
                        dumpstringdata(f, p->source->data, p->source->len);
                    fprintf(f, ":%d:%s\"", p->linedefined, p->debugname ? getstr(p->debugname) : "");
                }
            }
            else if (isLua(ci))
            {
                Proto* p = ci->p;
                int pc = pcRel(ci->savedpc, p);
                const LocVar* var = luaF_findlocal(p, int(v - ci->base), pc);

                if (var && var->varname)
                    fprintf(f, "\"%s\"", getstr(var->varname));
                else
                    fprintf(f, "null");
            }
            else
                fprintf(f, "null");
        }
        fprintf(f, "]");
    }
    fprintf(f, "}");
}

static void dumpbuffer(FILE* f, Buffer* b)
{
    fprintf(f, "{\"type\":\"buffer\",\"cat\":%d,\"size\":%d}", b->memcat, int(sizebuffer(b->len)));
}

static void dumpproto(FILE* f, Proto* p)
{
    size_t size = sizeof(Proto) + sizeof(Instruction) * p->sizecode + sizeof(Proto*) * p->sizep + sizeof(TValue) * p->sizek + p->sizelineinfo +
                  sizeof(LocVar) * p->sizelocvars + sizeof(TString*) * p->sizeupvalues;

    fprintf(f, "{\"type\":\"proto\",\"cat\":%d,\"size\":%d", p->memcat, int(size));

    if (p->source)
    {
        fprintf(f, ",\"source\":\"");
        dumpstringdata(f, p->source->data, p->source->len);
        fprintf(f, "\",\"line\":%d", p->abslineinfo ? p->abslineinfo[0] : 0);
    }

    if (p->sizek)
    {
        fprintf(f, ",\"constants\":[");
        dumprefs(f, p->k, p->sizek);
        fprintf(f, "]");
    }

    if (p->sizep)
    {
        fprintf(f, ",\"protos\":[");
        for (int i = 0; i < p->sizep; ++i)
        {
            if (i != 0)
                fputc(',', f);
            dumpref(f, obj2gco(p->p[i]));
        }
        fprintf(f, "]");
    }

    fprintf(f, "}");
}

static void dumpupval(FILE* f, UpVal* uv)
{
    fprintf(f, "{\"type\":\"upvalue\",\"cat\":%d,\"size\":%d,\"open\":%s", uv->memcat, int(sizeof(UpVal)), upisopen(uv) ? "true" : "false");

    if (iscollectable(uv->v))
    {
        fprintf(f, ",\"object\":");
        dumpref(f, gcvalue(uv->v));
    }

    fprintf(f, "}");
}

static void dumpclass(FILE* f, LuauClass* lco)
{
    fprintf(f, R"({"type":"class","cat":%d,"size":%d)", lco->memcat, int(sizeof(LuauClass)));
    fprintf(f, R"(,"name":)");
    dumpstringdata(f, lco->name->data, lco->name->len);
    fprintf(f, R"(,"super":)");
    if (lco->super)
        dumpref(f, obj2gco(lco->super));
    else
        fprintf(f, "null");
    fprintf(f, R"(,"membernames":[)");
    for (uint32_t i = 0; i < lco->numberofallmembers; i++)
    {
        if (i != 0)
            fputc(',', f);
        dumpref(f, (GCObject*)lco->offsettomember[i]);
    }
    fprintf(f, R"(],"staticmembers":[)");
    dumprefs(f, lco->staticmembers, lco->numberofallmembers - lco->numberofinstancemembers);
    fprintf(f, R"(],"metatable":)");
    if (lco->metatable)
        dumpref(f, obj2gco(lco->metatable));
    else
        fprintf(f, "null");
    fprintf(f, R"(,"instancemetatable":)");
    if (lco->instancemetatable)
        dumpref(f, obj2gco(lco->instancemetatable));
    else
        fprintf(f, "null");
    fprintf(f, R"(,"memberstooffset":)");
    dumpref(f, obj2gco(lco->memberstooffset));
    fprintf(f, "}");
}

static void dumpobject(FILE* f, LuauObject* inst)
{
    fprintf(f, R"({"type":"object","cat":%d,"size":%d)", inst->memcat, int(sizeof(LuauObject)));
    fprintf(f, R"(,"class":)");
    dumpref(f, obj2gco(inst->lclass));
    fprintf(f, R"(,"members":[)");
    dumprefs(f, inst->members, inst->numberofmembers);
    fprintf(f, "]}");
}

static void dumpobj(FILE* f, GCObject* o)
{
    switch (o->gch.tt)
    {
    case LUA_TSTRING:
        return dumpstring(f, gco2ts(o));

    case LUA_TTABLE:
        return dumptable(f, gco2h(o));

    case LUA_TFUNCTION:
        return dumpclosure(f, gco2cl(o));

    case LUA_TUSERDATA:
        return dumpudata(f, gco2u(o));

    case LUA_TTHREAD:
        return dumpthread(f, gco2th(o));

    case LUA_TVECTOR:
        return; // vector data is outlined, but is a constant cost of a vector

    case LUA_TBUFFER:
        return dumpbuffer(f, gco2buf(o));

    case LUA_TCLASS:
        return dumpclass(f, gco2class(o));

    case LUA_TOBJECT:
        return dumpobject(f, gco2object(o));

    case LUA_TPROTO:
        return dumpproto(f, gco2p(o));

    case LUA_TUPVAL:
        return dumpupval(f, gco2uv(o));

    default:
        LUAU_ASSERT(0);
    }
}

static bool dumpgco(void* context, lua_Page* page, GCObject* gco)
{
    FILE* f = (FILE*)context;

    dumpref(f, gco);
    fputc(':', f);
    dumpobj(f, gco);
    fputc(',', f);
    fputc('\n', f);

    return false;
}

void luaC_dump(lua_State* L, void* file, const char* (*categoryName)(lua_State* L, uint8_t memcat))
{
    global_State* g = L->global;
    FILE* f = static_cast<FILE*>(file);

    fprintf(f, "{\"objects\":{\n");

    dumpgco(f, NULL, obj2gco(g->mainthread));

    luaM_visitgco(L, f, dumpgco);

    fprintf(f, "\"0\":{\"type\":\"userdata\",\"cat\":0,\"size\":0}\n"); // to avoid issues with trailing ,
    fprintf(f, "},\"roots\":{\n");
    fprintf(f, "\"mainthread\":");
    dumpref(f, obj2gco(g->mainthread));
    fprintf(f, ",\"registry\":");
    dumpref(f, gcvalue(&g->registry));

    fprintf(f, "},\"stats\":{\n");

    fprintf(f, "\"size\":%d,\n", int(g->totalbytes));

    fprintf(f, "\"categories\":{\n");
    for (int i = 0; i < LUA_MEMORY_CATEGORIES; i++)
    {
        if (size_t bytes = g->memcatbytes[i])
        {
            if (categoryName)
                fprintf(f, "\"%d\":{\"name\":\"%s\", \"size\":%d},\n", i, categoryName(L, i), int(bytes));
            else
                fprintf(f, "\"%d\":{\"size\":%d},\n", i, int(bytes));
        }
    }
    fprintf(f, "\"none\":{}\n"); // to avoid issues with trailing ,
    fprintf(f, "}\n");
    fprintf(f, "}}\n");
}

struct EnumContext
{
    lua_State* L;
    void* context;
    void (*node)(void* context, void* ptr, uint8_t tt, uint8_t memcat, size_t size, const char* name);
    void (*edge)(void* context, void* from, void* to, const char* name);
};

static void* enumtopointer(GCObject* gco)
{
    // To match lua_topointer, userdata pointer is represented as a pointer to internal data
    return gco->gch.tt == LUA_TUSERDATA ? (void*)gco2u(gco)->data : (void*)gco;
}

static void enumnode(EnumContext* ctx, GCObject* gco, size_t size, const char* objname)
{
    ctx->node(ctx->context, enumtopointer(gco), gco->gch.tt, gco->gch.memcat, size, objname);
}

static void enumedge(EnumContext* ctx, GCObject* from, GCObject* to, const char* edgename)
{
    ctx->edge(ctx->context, enumtopointer(from), enumtopointer(to), edgename);
}

static void enumedges(EnumContext* ctx, GCObject* from, TValue* data, size_t size, const char* edgename)
{
    for (size_t i = 0; i < size; ++i)
    {
        if (iscollectable(&data[i]))
            enumedge(ctx, from, gcvalue(&data[i]), edgename);
    }
}

static void enumstring(EnumContext* ctx, TString* ts)
{
    enumnode(ctx, obj2gco(ts), sizestring(ts->len), NULL);
}

static void enumtable(EnumContext* ctx, LuaTable* h)
{
    size_t size = sizeof(LuaTable) + (h->node == &luaH_dummynode ? 0 : sizenode(h) * sizeof(LuaNode)) + h->sizearray * sizeof(TValue);

    if (FFlag::LuauFrozenMetaButterfly && hasmetacache(h))
        size += TM_N * sizeof(TValue);

    // Provide a name for a special registry table
    enumnode(ctx, obj2gco(h), size, h == hvalue(registry(ctx->L)) ? "registry" : NULL);

    if (h->node != &luaH_dummynode)
    {
        bool weakkey = false;
        bool weakvalue = false;

        if (const TValue* mode = gfasttm(ctx->L->global, h->metatable, TM_MODE))
        {
            if (ttisstring(mode))
            {
                weakkey = strchr(svalue(mode), 'k') != NULL;
                weakvalue = strchr(svalue(mode), 'v') != NULL;
            }
        }

        for (int i = 0; i < sizenode(h); ++i)
        {
            const LuaNode& n = h->node[i];

            if (!ttisnil(&n.val) && (iscollectable(&n.key) || iscollectable(&n.val)))
            {
                if (!weakkey && iscollectable(&n.key))
                    enumedge(ctx, obj2gco(h), gcvalue(&n.key), "[key]");

                if (!weakvalue && iscollectable(&n.val))
                {
                    if (ttisstring(&n.key))
                    {
                        enumedge(ctx, obj2gco(h), gcvalue(&n.val), svalue(&n.key));
                    }
                    else if (ttisnumber(&n.key))
                    {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%.14g", nvalue(&n.key));
                        enumedge(ctx, obj2gco(h), gcvalue(&n.val), buf);
                    }
                    else
                    {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "[%s]", getstr(ctx->L->global->ttname[n.key.tt]));
                        enumedge(ctx, obj2gco(h), gcvalue(&n.val), buf);
                    }
                }
            }
        }
    }

    if (h->sizearray)
        enumedges(ctx, obj2gco(h), h->array, h->sizearray, "array");

    if (h->metatable)
        enumedge(ctx, obj2gco(h), obj2gco(h->metatable), "metatable");
}

static void enumclosure(EnumContext* ctx, Closure* cl)
{
    if (cl->isC)
    {
        enumnode(ctx, obj2gco(cl), sizeCclosure(cl->nupvalues), cl->c.debugname ? getstr(cl->c.debugname) : nullptr);

        if (FFlag::LuauEnumMoreEdges && cl->c.debugname)
            enumedge(ctx, obj2gco(cl), obj2gco(cl->c.debugname), "name");
    }
    else
    {
        Proto* p = cl->l.p;

        char buf[LUA_IDSIZE];

        if (p->source)
            snprintf(buf, sizeof(buf), "%s:%d %s", p->debugname ? getstr(p->debugname) : "unnamed", p->linedefined, getstr(p->source));
        else
            snprintf(buf, sizeof(buf), "%s:%d", p->debugname ? getstr(p->debugname) : "unnamed", p->linedefined);

        enumnode(ctx, obj2gco(cl), sizeLclosure(cl->nupvalues), buf);
    }

    enumedge(ctx, obj2gco(cl), obj2gco(cl->env), "env");

    if (cl->isC)
    {
        if (cl->nupvalues)
            enumedges(ctx, obj2gco(cl), cl->c.upvals, cl->nupvalues, "upvalue");
    }
    else
    {
        enumedge(ctx, obj2gco(cl), obj2gco(cl->l.p), "proto");

        if (cl->nupvalues)
            enumedges(ctx, obj2gco(cl), cl->l.uprefs, cl->nupvalues, "upvalue");
    }
}

static void enumudata(EnumContext* ctx, Udata* u)
{
    const char* name = NULL;

    if (LuaTable* h = u->metatable)
    {
        if (h->node != &luaH_dummynode)
        {
            for (int i = 0; i < sizenode(h); ++i)
            {
                const LuaNode& n = h->node[i];

                if (ttisstring(&n.key) && ttisstring(&n.val) && strcmp(svalue(&n.key), "__type") == 0)
                {
                    name = svalue(&n.val);
                    break;
                }
            }
        }
    }

    enumnode(ctx, obj2gco(u), sizeudata(u->len), name);

    if (u->metatable)
        enumedge(ctx, obj2gco(u), obj2gco(u->metatable), "metatable");
}

static void enumthread(EnumContext* ctx, lua_State* th)
{
    size_t size = sizeof(lua_State) + sizeof(TValue) * th->stacksize + sizeof(CallInfo) * th->size_ci;

    Proto* cip = NULL;
    for (CallInfo* ci = th->base_ci; ci <= th->ci; ++ci)
    {
        if (ttisfunction(ci->func))
        {
            cip = ci->p;
            break;
        }
    }

    if (cip && cip->source)
    {
        Proto* p = cip;

        char buf[LUA_IDSIZE];

        if (p->source)
            snprintf(buf, sizeof(buf), "thread at %s:%d %s", p->debugname ? getstr(p->debugname) : "unnamed", p->linedefined, getstr(p->source));
        else
            snprintf(buf, sizeof(buf), "thread at %s:%d", p->debugname ? getstr(p->debugname) : "unnamed", p->linedefined);

        enumnode(ctx, obj2gco(th), size, buf);
    }
    else
    {
        enumnode(ctx, obj2gco(th), size, NULL);
    }

    enumedge(ctx, obj2gco(th), obj2gco(th->gt), "globals");

    if (th->finalizers)
        enumedge(ctx, obj2gco(th), obj2gco(th->finalizers), "finalizers");

    if (th->top > th->stack)
        enumedges(ctx, obj2gco(th), th->stack, th->top - th->stack, "stack");
}

static void enumbuffer(EnumContext* ctx, Buffer* b)
{
    enumnode(ctx, obj2gco(b), sizebuffer(b->len), NULL);
}

static void enumproto(EnumContext* ctx, Proto* p)
{
    size_t size = sizeof(Proto) + sizeof(Instruction) * p->sizecode + sizeof(Proto*) * p->sizep + sizeof(TValue) * p->sizek + p->sizelineinfo +
                  sizeof(LocVar) * p->sizelocvars + sizeof(TString*) * p->sizeupvalues;

    if (p->execdata && ctx->L->global->ecb.getmemorysize)
    {
        size_t nativesize = ctx->L->global->ecb.getmemorysize(ctx->L, p);

        ctx->node(ctx->context, p->execdata, uint8_t(LUA_TNONE), p->memcat, nativesize, NULL);
        ctx->edge(ctx->context, enumtopointer(obj2gco(p)), p->execdata, "[native]");
    }

    char buf[LUA_IDSIZE];

    if (p->source)
        snprintf(buf, sizeof(buf), "proto %s:%d %s", p->debugname ? getstr(p->debugname) : "unnamed", p->linedefined, getstr(p->source));
    else
        snprintf(buf, sizeof(buf), "proto %s:%d", p->debugname ? getstr(p->debugname) : "unnamed", p->linedefined);

    enumnode(ctx, obj2gco(p), size, buf);

    if (p->sizek)
        enumedges(ctx, obj2gco(p), p->k, p->sizek, "constants");

    for (int i = 0; i < p->sizep; ++i)
        enumedge(ctx, obj2gco(p), obj2gco(p->p[i]), "protos");

    if (FFlag::LuauEnumMoreEdges)
    {
        if (p->debugname)
            enumedge(ctx, obj2gco(p), obj2gco(p->debugname), "name");

        if (p->source)
            enumedge(ctx, obj2gco(p), obj2gco(p->source), "source");

        for (int i = 0; i < p->sizelocvars; i++)
        {
            if (TString* str = p->locvars[i].varname)
                enumedge(ctx, obj2gco(p), obj2gco(str), "[local name]");
        }

        for (int i = 0; i < p->sizeupvalues; ++i)
        {
            if (TString* str = p->upvalues[i])
                enumedge(ctx, obj2gco(p), obj2gco(str), "[upvalue name]");
        }

        if (p->optimized)
            enumedge(ctx, obj2gco(p), obj2gco(p->optimized), "optimized");

        if (p->deoptimized)
            enumedge(ctx, obj2gco(p), obj2gco(p->deoptimized), "deoptimized");
    }
}

static void enumupval(EnumContext* ctx, UpVal* uv)
{
    enumnode(ctx, obj2gco(uv), sizeof(UpVal), NULL);

    if (iscollectable(uv->v))
        enumedge(ctx, obj2gco(uv), gcvalue(uv->v), "value");
}

static void enumclass(EnumContext* ctx, LuauClass* lco)
{
    GCObject* obj = obj2gco(lco);

    char buf[LUA_IDSIZE];
    snprintf(buf, sizeof(buf), "class object %s", getstr(lco->name));
    enumnode(ctx, obj, sizeof(LuauClass), buf);
    enumedge(ctx, obj, obj2gco(lco->name), "classname");

    if (lco->super)
        enumedge(ctx, obj, obj2gco(lco->super), "super");

    enumedge(ctx, obj, obj2gco(lco->memberstooffset), "classoffsets");
    uint32_t numberofstaticmembers = lco->numberofallmembers - lco->numberofinstancemembers;

    for (uint32_t i = 0; i < numberofstaticmembers; i++)
    {
        if (!iscollectable(&lco->staticmembers[i]))
            continue;

        char membername[32];
        snprintf(membername, sizeof(membername), "%s", getstr(lco->offsettomember[i + lco->numberofinstancemembers]));
        enumedge(ctx, obj, gcvalue(&lco->staticmembers[i]), membername);
    }

    for (uint32_t i = 0; i < lco->numberofallmembers; i++)
        enumedge(ctx, obj, obj2gco(lco->offsettomember[i]), "membername");

    if (lco->metatable)
        enumedge(ctx, obj, obj2gco(lco->metatable), "metatable");

    if (FFlag::LuauEnumMoreEdges && lco->instancemetatable)
        enumedge(ctx, obj, obj2gco(lco->instancemetatable), "instancemetatable");
}

static void enumobject(EnumContext* ctx, LuauObject* inst)
{
    GCObject* obj = obj2gco(inst);

    char buf[LUA_IDSIZE];
    snprintf(buf, sizeof(buf), "object %s", getstr(inst->lclass->name));
    enumnode(ctx, obj, sizeof(LuauObject), buf);

    for (uint32_t i = 0; i < inst->lclass->numberofinstancemembers; i++)
    {
        if (!iscollectable(&inst->members[i]))
            continue;

        char membername[32];
        snprintf(membername, sizeof(membername), "%s", getstr(inst->lclass->offsettomember[i]));
        enumedge(ctx, obj, gcvalue(&inst->members[i]), membername);
    }

    if (FFlag::LuauEnumMoreEdges && inst->lclass)
        enumedge(ctx, obj, obj2gco(inst->lclass), "class");
}

static void enumobj(EnumContext* ctx, GCObject* o)
{
    switch (o->gch.tt)
    {
    case LUA_TSTRING:
        return enumstring(ctx, gco2ts(o));

    case LUA_TTABLE:
        return enumtable(ctx, gco2h(o));

    case LUA_TFUNCTION:
        return enumclosure(ctx, gco2cl(o));

    case LUA_TUSERDATA:
        return enumudata(ctx, gco2u(o));

    case LUA_TTHREAD:
        return enumthread(ctx, gco2th(o));

    case LUA_TVECTOR:
        return enumnode(ctx, o, sizeof(LuauVector), NULL);

    case LUA_TBUFFER:
        return enumbuffer(ctx, gco2buf(o));

    case LUA_TCLASS:
        return enumclass(ctx, gco2class(o));

    case LUA_TOBJECT:
        return enumobject(ctx, gco2object(o));

    case LUA_TPROTO:
        return enumproto(ctx, gco2p(o));

    case LUA_TUPVAL:
        return enumupval(ctx, gco2uv(o));

    default:
        LUAU_ASSERT(!"Unknown object tag");
    }
}

static bool enumgco(void* context, lua_Page* page, GCObject* gco)
{
    enumobj((EnumContext*)context, gco);
    return false;
}

void luaC_enumheap(
    lua_State* L,
    void* context,
    void (*node)(void* context, void* ptr, uint8_t tt, uint8_t memcat, size_t size, const char* name),
    void (*edge)(void* context, void* from, void* to, const char* name)
)
{
    global_State* g = L->global;

    EnumContext ctx;
    ctx.L = L;
    ctx.context = context;
    ctx.node = node;
    ctx.edge = edge;

    enumgco(&ctx, NULL, obj2gco(g->mainthread));

    // global state links from mainthread
    if (FFlag::LuauEnumMoreEdges)
    {
        if (iscollectable(&g->weakregistry))
            enumedge(&ctx, obj2gco(g->mainthread), gcvalue(&g->weakregistry), "weakregistry");

        for (int i = 0; i < LUA_T_COUNT; i++)
        {
            if (g->mt[i])
                enumedge(&ctx, obj2gco(g->mainthread), obj2gco(g->mt[i]), "[type metatable]");

            enumedge(&ctx, obj2gco(g->mainthread), obj2gco(g->ttname[i]), "[type name]");
        }

        for (int i = 0; i < TM_N; i++)
            enumedge(&ctx, obj2gco(g->mainthread), obj2gco(g->tmname[i]), "[metamethod name]");

        for (int i = 0; i < LUA_UTAG_LIMIT; i++)
        {
            if (g->udatamt[i])
                enumedge(&ctx, obj2gco(g->mainthread), obj2gco(g->udatamt[i]), "[userdata metatable]");
        }

        for (int i = 0; i < LUA_LUTAG_LIMIT; i++)
        {
            if (g->lightuserdataname[i])
                enumedge(&ctx, obj2gco(g->mainthread), obj2gco(g->lightuserdataname[i]), "[lightuserdata name]");
        }

        for (int i = 0; i < UTAG_INTERNAL_LIMIT; i++)
        {
            lua_UdataDirectAccessData& direct = g->udatadirect[i];

            if (iscollectable(&direct.indextm))
                enumedge(&ctx, obj2gco(g->mainthread), gcvalue(&direct.indextm), "[direct userdata]");

            if (iscollectable(&direct.newindextm))
                enumedge(&ctx, obj2gco(g->mainthread), gcvalue(&direct.newindextm), "[direct userdata]");

            if (iscollectable(&direct.namecalltm))
                enumedge(&ctx, obj2gco(g->mainthread), gcvalue(&direct.namecalltm), "[direct userdata]");

            if (g->udatadirectfields[i])
                enumedge(&ctx, obj2gco(g->mainthread), obj2gco(g->udatadirectfields[i]), "[direct userdata]");
        }

        enumedge(&ctx, obj2gco(g->mainthread), obj2gco(luaS_newliteral(L, LUA_MEMERRMSG)), "[fixed string]");
        enumedge(&ctx, obj2gco(g->mainthread), obj2gco(luaS_newliteral(L, LUA_ERRERRMSG)), "[fixed string]");
    }

    luaM_visitgco(L, &ctx, enumgco);
}
