// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "ldebug.h"

#include "lapi.h"
#include "lfunc.h"
#include "lmem.h"
#include "lgc.h"
#include "ldo.h"
#include "lbytecode.h"
#include "lvm.h"

#include <string.h>
#include <stdio.h>

static const char* getfuncname(Closure* cl);

static int currentpc(lua_State* L, CallInfo* ci)
{
    return pcRel(ci->savedpc, ci->p);
}

static int currentline(lua_State* L, CallInfo* ci)
{
    return luaG_getline(ci->p, currentpc(L, ci));
}

static Proto* getluaproto(CallInfo* ci)
{
    return cast_to(Proto*, ci->p);
}

int lua_getargument(lua_State* L, int level, int n)
{
    if (unsigned(level) >= unsigned(L->ci - L->base_ci))
        return 0;

    CallInfo* ci = L->ci - level;
    // changing tables in native functions externally may invalidate safety contracts wrt table state (metatable/size/readonly)
    if (ci->flags & LUA_CALLINFO_NATIVE)
        return 0;

    Proto* fp = getluaproto(ci);
    int res = 0;

    if (fp && n > 0)
    {
        if (n <= fp->numparams)
        {
            luaC_threadbarrier(L);
            luaA_pushvalue(L, ci->base + (n - 1));
            res = 1;
        }
        else if (fp->is_vararg && n < ci->base - ci->func)
        {
            luaC_threadbarrier(L);
            luaA_pushvalue(L, ci->func + n);
            res = 1;
        }
    }

    return res;
}

const char* lua_getlocal(lua_State* L, int level, int n)
{
    if (unsigned(level) >= unsigned(L->ci - L->base_ci))
        return NULL;

    CallInfo* ci = L->ci - level;
    // changing tables in native functions externally may invalidate safety contracts wrt table state (metatable/size/readonly)
    if (ci->flags & LUA_CALLINFO_NATIVE)
        return NULL;

    Proto* fp = getluaproto(ci);
    const LocVar* var = fp ? luaF_getlocal(fp, n, currentpc(L, ci)) : NULL;
    if (var)
    {
        luaC_threadbarrier(L);
        luaA_pushvalue(L, ci->base + var->reg);
    }
    const char* name = var ? getstr(var->varname) : NULL;
    return name;
}

const char* lua_setlocal(lua_State* L, int level, int n)
{
    api_check(L, L->top - L->base >= 1);

    if (unsigned(level) >= unsigned(L->ci - L->base_ci))
        return NULL;

    CallInfo* ci = L->ci - level;
    // changing registers in native functions externally may invalidate safety contracts wrt register type tags
    if (ci->flags & LUA_CALLINFO_NATIVE)
        return NULL;

    Proto* fp = getluaproto(ci);
    const LocVar* var = fp ? luaF_getlocal(fp, n, currentpc(L, ci)) : NULL;
    if (var)
        setobj2s(L, ci->base + var->reg, L->top - 1);
    L->top--; // pop value
    const char* name = var ? getstr(var->varname) : NULL;
    return name;
}

static Closure* auxgetinfo(lua_State* L, const char* what, lua_Debug* ar, Closure* f, CallInfo* ci)
{
    Closure* cl = NULL;
    for (; *what; what++)
    {
        switch (*what)
        {
        case 's':
        {
            if (f->isC)
            {
                ar->source = "=[C]";
                ar->what = "C";
                ar->linedefined = -1;
                ar->short_src = "[C]";
            }
            else
            {
                TString* source = (ci != nullptr ? ci->p : f->l.p)->source;
                ar->source = getstr(source);
                ar->what = "Lua";
                ar->linedefined = (ci != nullptr ? ci->p : f->l.p)->linedefined;
                ar->short_src = luaO_chunkid(ar->ssbuf, sizeof(ar->ssbuf), getstr(source), source->len);
            }
            break;
        }
        case 'l':
        {
            if (ci)
            {
                ar->currentline = isLua(ci) ? currentline(L, ci) : -1;
            }
            else
            {
                ar->currentline = f->isC ? -1 : f->l.p->linedefined;
            }

            break;
        }
        case 'u':
        {
            ar->nupvals = f->nupvalues;
            break;
        }
        case 'a':
        {
            if (f->isC)
            {
                ar->isvararg = 1;
                ar->nparams = 0;
            }
            else
            {
                ar->isvararg = (ci != nullptr ? ci->p : f->l.p)->is_vararg;
                ar->nparams = (ci != nullptr ? ci->p : f->l.p)->numparams;
            }
            break;
        }
        case 'p':
        {
            if (f->isC)
            {
                ar->protoid = 0;
                ar->bytecodeid = -1;
            }
            else
            {
                Proto* p = ci != nullptr ? ci->p : f->l.p;
                ar->protoid = int(p->funid);
                ar->bytecodeid = p->bytecodeid;
            }
            break;
        }
        case 'n':
        {
            ar->name = ci ? getfuncname(ci_func(ci)) : getfuncname(f);
            break;
        }
        case 'f':
        {
            cl = f;
            break;
        }
        default:;
        }
    }
    return cl;
}

void lua_callhook(lua_State* L, lua_Hook hook, void* userdata)
{
    api_check(L, hook != nullptr);
    api_check(L, L->ci != L->base_ci);

    return luau_callhook(L, hook, userdata);
}

int lua_stackdepth(lua_State* L)
{
    return int(L->ci - L->base_ci);
}

int lua_getinfo(lua_State* L, int level, const char* what, lua_Debug* ar)
{
    Closure* f = NULL;
    CallInfo* ci = NULL;
    if (level < 0)
    {
        // element has to be within stack
        if (-level > L->top - L->base)
            return 0;

        StkId func = L->top + level;

        // and it has to be a function
        if (!ttisfunction(func))
            return 0;

        f = clvalue(func);
    }
    else if (unsigned(level) < unsigned(L->ci - L->base_ci))
    {
        ci = L->ci - level;
        LUAU_ASSERT(ttisfunction(ci->func));
        f = clvalue(ci->func);
    }
    if (f)
    {
        // auxgetinfo fills ar and optionally requests to put closure on stack
        if (Closure* fcl = auxgetinfo(L, what, ar, f, ci))
        {
            luaC_threadbarrier(L);
            setclvalue(L, L->top, fcl);
            incr_top(L);
        }
    }
    return f ? 1 : 0;
}

static const char* getfuncname(Closure* cl)
{
    if (cl->isC)
    {
        if (TString* str = cl->c.debugname)
            return getstr(str);
    }
    else
    {
        Proto* p = cl->l.p;

        if (p->debugname)
        {
            return getstr(p->debugname);
        }
    }
    return nullptr;
}

l_noret luaG_typeerrorL(lua_State* L, const TValue* o, const char* op)
{
    const char* t = luaT_objtypename(L, o);

    luaG_runerror(L, "attempt to %s a %s value", op, t);
}

l_noret luaG_forerrorL(lua_State* L, const TValue* o, const char* what)
{
    const char* t = luaT_objtypename(L, o);

    luaG_runerror(L, "invalid 'for' %s (number expected, got %s)", what, t);
}

l_noret luaG_concaterror(lua_State* L, StkId p1, StkId p2)
{
    const char* t1 = luaT_objtypename(L, p1);
    const char* t2 = luaT_objtypename(L, p2);

    luaG_runerror(L, "attempt to concatenate %s with %s", t1, t2);
}

l_noret luaG_aritherror(lua_State* L, const TValue* p1, const TValue* p2, TMS op)
{
    const char* t1 = luaT_objtypename(L, p1);
    const char* t2 = luaT_objtypename(L, p2);
    const char* opname = luaT_eventname[op] + 2; // skip __ from metamethod name

    if (t1 == t2)
        luaG_runerror(L, "attempt to perform arithmetic (%s) on %s", opname, t1);
    else
        luaG_runerror(L, "attempt to perform arithmetic (%s) on %s and %s", opname, t1, t2);
}

l_noret luaG_ordererror(lua_State* L, const TValue* p1, const TValue* p2, TMS op)
{
    const char* t1 = luaT_objtypename(L, p1);
    const char* t2 = luaT_objtypename(L, p2);
    const char* opname = (op == TM_LT) ? "<" : (op == TM_LE) ? "<=" : "==";

    luaG_runerror(L, "attempt to compare %s %s %s", t1, opname, t2);
}

l_noret luaG_indexerror(lua_State* L, const TValue* p1, const TValue* p2)
{
    const char* t1 = luaT_objtypename(L, p1);
    const char* t2 = luaT_objtypename(L, p2);
    const TString* key = ttisstring(p2) ? tsvalue(p2) : 0;

    if (key && key->len <= 64) // limit length to make sure we don't generate very long error messages for very long keys
        luaG_runerror(L, "attempt to index %s with '%s'", t1, getstr(key));
    else
        luaG_runerror(L, "attempt to index %s with %s", t1, t2);
}

l_noret luaG_missingmembererror(lua_State* L, const TValue* p1, const TValue* p2)
{
    if (!ttisstring(p2))
        luaG_runerrorL(L, "cannot index %s with a %s", luaT_objtypename(L, p1), luaT_objtypename(L, p2));
    else
        luaG_runerrorL(L, "this %s does not have a key named '%s'", luaT_objtypename(L, p1), getstr(tsvalue(p2)));
}

l_noret luaG_methoderror(lua_State* L, const TValue* p1, const TString* p2)
{
    const char* t1 = luaT_objtypename(L, p1);

    luaG_runerror(L, "attempt to call missing method '%s' of %s", getstr(p2), t1);
}

l_noret luaG_readonlyerror(lua_State* L)
{
    luaG_runerror(L, "attempt to modify a readonly table");
}

static void pusherror(lua_State* L, const char* msg)
{
    CallInfo* ci = L->ci;
    if (isLua(ci))
    {
        TString* source = getluaproto(ci)->source;
        char chunkbuf[LUA_IDSIZE]; // add file:line information
        const char* chunkid = luaO_chunkid(chunkbuf, sizeof(chunkbuf), getstr(source), source->len);
        int line = currentline(L, ci);
        luaO_pushfstring(L, "%s:%d: %s", chunkid, line, msg);
    }
    else
    {
        lua_pushstring(L, msg);
    }
}

l_noret luaG_runerrorL(lua_State* L, const char* fmt, ...)
{
    va_list argp;
    va_start(argp, fmt);
    char result[LUA_BUFFERSIZE];
    vsnprintf(result, sizeof(result), fmt, argp);
    va_end(argp);

    lua_rawcheckstack(L, 1);

    pusherror(L, result);
    luaD_throw(L, LUA_ERRRUN);
}

void luaG_pusherror(lua_State* L, const char* error)
{
    lua_rawcheckstack(L, 1);

    pusherror(L, error);
}

void luaG_breakpoint(lua_State* L, Proto* p, int line, bool enable)
{
    void (*ondisable)(lua_State*, Proto*) = L->global->ecb.disable;

    // since native code doesn't support breakpoints, we would need to update all call frames with LUAU_CALLINFO_NATIVE that refer to p
    if (p->lineinfo && (ondisable || !p->execdata))
    {
        for (int i = 0; i < p->sizecode; ++i)
        {
            // note: we keep prologue as is, instead opting to break at the first meaningful instruction
            if (LUAU_INSN_OP(p->code[i]) == LOP_PREPVARARGS)
                continue;

            if (luaG_getline(p, i) != line)
                continue;

            // lazy copy of the original opcode array; done when the first breakpoint is set
            if (!p->debuginsn)
            {
                p->debuginsn = luaM_newarray(L, p->sizecode, uint8_t, p->memcat);
                for (int j = 0; j < p->sizecode; ++j)
                    p->debuginsn[j] = LUAU_INSN_OP(p->code[j]);
            }

            uint8_t op = enable ? LOP_BREAK : LUAU_INSN_OP(p->debuginsn[i]);

            // patch just the opcode byte, leave arguments alone
            p->code[i] &= ~0xff;
            p->code[i] |= op;
            LUAU_ASSERT(LUAU_INSN_OP(p->code[i]) == op);

            // currently we don't restore native code when breakpoint is disabled.
            // this will be addressed in the future.
            if (enable && p->execdata && ondisable)
                ondisable(L, p);

            // note: this is important!
            // we only patch the *first* instruction in each proto that's attributed to a given line
            // this can be changed, but if requires making patching a bit more nuanced so that we don't patch AUX words
            break;
        }
    }

    for (int i = 0; i < p->sizep; ++i)
    {
        luaG_breakpoint(L, p->p[i], line, enable);
    }
}

bool luaG_onbreak(lua_State* L)
{
    if (L->ci == L->base_ci)
        return false;

    if (!isLua(L->ci))
        return false;

    return LUAU_INSN_OP(*L->ci->savedpc) == LOP_BREAK;
}

int luaG_getline(Proto* p, int pc)
{
    LUAU_ASSERT(pc >= 0 && pc < p->sizecode);

    if (!p->lineinfo)
        return 0;

    return p->abslineinfo[pc >> p->linegaplog2] + p->lineinfo[pc];
}

int luaG_isnative(lua_State* L, int level)
{
    if (unsigned(level) >= unsigned(L->ci - L->base_ci))
        return 0;

    CallInfo* ci = L->ci - level;
    return (ci->flags & LUA_CALLINFO_NATIVE) != 0 ? 1 : 0;
}

int luaG_hasnative(lua_State* L, int level)
{
    if (unsigned(level) >= unsigned(L->ci - L->base_ci))
        return 0;

    CallInfo* ci = L->ci - level;

    Proto* proto = getluaproto(ci);
    if (proto == nullptr)
        return 0;

    return (proto->execdata != nullptr);
}

void lua_singlestep(lua_State* L, int enabled)
{
    L->singlestep = bool(enabled);
}

static int getmaxline(Proto* p)
{
    int result = -1;

    for (int i = 0; i < p->sizecode; ++i)
    {
        int line = luaG_getline(p, i);
        result = result < line ? line : result;
    }

    for (int i = 0; i < p->sizep; ++i)
    {
        int psize = getmaxline(p->p[i]);
        result = result < psize ? psize : result;
    }

    return result;
}

// Find the line number with instructions. If the provided line doesn't have any instruction, it should return the next valid line number.
static int getnextline(Proto* p, int line)
{
    int closest = -1;

    if (p->lineinfo)
    {
        for (int i = 0; i < p->sizecode; ++i)
        {
            // note: we keep prologue as is, instead opting to break at the first meaningful instruction
            if (LUAU_INSN_OP(p->code[i]) == LOP_PREPVARARGS)
                continue;

            int candidate = luaG_getline(p, i);

            if (candidate == line)
                return line;

            if (candidate > line && (closest == -1 || candidate < closest))
                closest = candidate;
        }
    }

    for (int i = 0; i < p->sizep; ++i)
    {
        int candidate = getnextline(p->p[i], line);

        if (candidate == line)
            return line;

        if (candidate > line && (closest == -1 || candidate < closest))
            closest = candidate;
    }

    return closest;
}

int lua_breakpoint(lua_State* L, int funcindex, int line, int enabled)
{
    const TValue* func = luaA_toobject(L, funcindex);
    api_check(L, ttisfunction(func) && !clvalue(func)->isC);

    Proto* p = clvalue(func)->l.p;

    // set the breakpoint to the next closest line with valid instructions
    int target = getnextline(p, line);

    if (target != -1)
        luaG_breakpoint(L, p, target, bool(enabled));

    return target;
}

int lua_atbreakpoint(lua_State* L)
{
    return luaG_onbreak(L) ? 1 : 0;
}

static void getcoverage(Proto* p, int depth, int* buffer, size_t size, void* context, lua_Coverage callback)
{
    memset(buffer, -1, size * sizeof(int));

    for (int i = 0; i < p->sizecode; ++i)
    {
        Instruction insn = p->code[i];
        if (LUAU_INSN_OP(insn) != LOP_COVERAGE)
            continue;

        int line = luaG_getline(p, i);
        int hits = LUAU_INSN_E(insn);

        LUAU_ASSERT(size_t(line) < size);
        buffer[line] = buffer[line] < hits ? hits : buffer[line];
    }

    const char* debugname = p->debugname ? getstr(p->debugname) : NULL;
    int linedefined = p->linedefined;

    callback(context, debugname, linedefined, depth, buffer, size);

    for (int i = 0; i < p->sizep; ++i)
        getcoverage(p->p[i], depth + 1, buffer, size, context, callback);
}

void lua_getcoverage(lua_State* L, int funcindex, void* context, lua_Coverage callback)
{
    const TValue* func = luaA_toobject(L, funcindex);
    api_check(L, ttisfunction(func) && !clvalue(func)->isC);

    Proto* p = clvalue(func)->l.p;

    size_t size = getmaxline(p) + 1;
    if (size == 0)
        return;

    int* buffer = luaM_newarray(L, size, int, 0);

    getcoverage(p, 0, buffer, size, context, callback);

    luaM_freearray(L, buffer, size, int, 0);
}

static void getcounters(lua_State* L, Proto* p, void* context, lua_CounterFunction functionvisit, lua_CounterValue countervisit)
{
    if (p->execdata != nullptr && L->global->ecb.getcounterdata != nullptr)
    {
        size_t count = 0;
        char* data = L->global->ecb.getcounterdata(L, p, &count);

        if (data != nullptr && count != 0)
        {
            const char* debugname = p->debugname ? getstr(p->debugname) : nullptr;
            int linedefined = p->linedefined;

            functionvisit(context, debugname, linedefined);

            for (size_t i = 0; i < count; i++)
            {
                uint32_t kind = 0;
                memcpy(&kind, data + 0, sizeof(kind));
                data += sizeof(kind);

                uint32_t pcpos = 0;
                memcpy(&pcpos, data + 0, sizeof(pcpos));
                data += sizeof(pcpos);

                uint64_t hits = 0;
                memcpy(&hits, data + 0, sizeof(hits));
                data += sizeof(hits);

                int line = pcpos == ~0u ? p->linedefined : luaG_getline(p, pcpos);

                countervisit(context, kind, line, hits);
            }
        }
    }

    for (int i = 0; i < p->sizep; ++i)
        getcounters(L, p->p[i], context, functionvisit, countervisit);
}

void lua_getcounters(lua_State* L, int funcindex, void* context, lua_CounterFunction functionvisit, lua_CounterValue countervisit)
{
    const TValue* func = luaA_toobject(L, funcindex);
    api_check(L, ttisfunction(func) && !clvalue(func)->isC);

    if (L->global->ecb.getcounterdata == nullptr)
        return;

    Proto* p = clvalue(func)->l.p;

    getcounters(L, p, context, functionvisit, countervisit);
}

static size_t append(char* buf, size_t bufsize, size_t offset, const char* data)
{
    size_t size = strlen(data);
    size_t copy = offset + size >= bufsize ? bufsize - offset - 1 : size;
    memcpy(buf + offset, data, copy);
    return offset + copy;
}

const char* lua_debugtrace(lua_State* L)
{
    static char buf[4096];

    const int limit1 = 10;
    const int limit2 = 10;

    int depth = int(L->ci - L->base_ci);
    size_t offset = 0;

    lua_Debug ar;
    for (int level = 0; lua_getinfo(L, level, "sln", &ar); ++level)
    {
        if (ar.source)
            offset = append(buf, sizeof(buf), offset, ar.short_src);

        if (ar.currentline > 0)
        {
            char line[32];
            snprintf(line, sizeof(line), ":%d", ar.currentline);

            offset = append(buf, sizeof(buf), offset, line);
        }

        if (ar.name)
        {
            offset = append(buf, sizeof(buf), offset, " function ");
            offset = append(buf, sizeof(buf), offset, ar.name);
        }

        offset = append(buf, sizeof(buf), offset, "\n");

        if (depth > limit1 + limit2 && level == limit1 - 1)
        {
            char skip[32];
            snprintf(skip, sizeof(skip), "... (+%d frames)\n", int(depth - limit1 - limit2));

            offset = append(buf, sizeof(buf), offset, skip);

            level = depth - limit2 - 1;
        }
    }

    LUAU_ASSERT(offset < sizeof(buf));
    buf[offset] = '\0';

    return buf;
}
