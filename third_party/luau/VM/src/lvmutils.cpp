// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "lclass.h"
#include "lfunc.h"
#include "lvm.h"

#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lvector.h"
#include "lgc.h"
#include "ldo.h"
#include "lnumutils.h"

#include <string.h>

// limit for table tag-method chains (to avoid loops)
#define MAXTAGLOOP 100

LUAU_FASTFLAG(LuauFrozenMetaButterfly)
LUAU_FASTFLAG(DebugLuauUserDefinedClassesRuntime)
LUAU_FASTFLAG(LuauPromoteProto)
LUAU_FASTFLAGVARIABLE(LuauCallLuauTm)

const TValue* luaV_tonumber(const TValue* obj, TValue* n)
{
    double num;
    if (ttisnumber(obj))
        return obj;
    if (ttisstring(obj) && luaO_str2d(svalue(obj), &num))
    {
        setnvalue(n, num);
        return n;
    }
    else
        return NULL;
}

int luaV_tostring(lua_State* L, StkId obj)
{
    if (!ttisnumber(obj))
        return 0;
    else
    {
        char s[LUAI_MAXNUM2STR];
        double n = nvalue(obj);
        char* e = luai_num2str(s, n);
        LUAU_ASSERT(e < s + sizeof(s));
        setsvalue(L, obj, luaS_newlstr(L, s, e - s));
        return 1;
    }
}

const LUA_VECTOR_TYPE* luaV_tovector(const TValue* obj)
{
    if (ttisvector(obj))
        return vvalue(obj);

    return nullptr;
}

static LUAU_FORCEINLINE void callTMluau(lua_State* L, StkId top, Closure* ccl, int nresults)
{
    if (++L->nCcalls >= LUAI_MAXCCALLS)
        luaD_checkCstack(L);

    ptrdiff_t funcoffset = savestack(L, top);
    Proto* p = getproto(ccl);

    CallInfo* ci = incr_ci(L);
    ci->func = top;
    ci->p = p;
    ci->base = top + 1;
    ci->top = L->top + ccl->stacksize;
    ci->savedpc = p->code;
    ci->flags = LUA_CALLINFO_RETURN;
    ci->nresults = nresults;

    L->base = ci->base;

    luaD_checkstackfornewci(L, ccl->stacksize); // clobbers 'top'
    LUAU_ASSERT(ci->top <= L->stack_last);

    StkId argi = L->top;
    StkId argend = L->base + p->numparams;
    while (argi < argend)
        setnilvalue(argi++);
    L->top = p->is_vararg ? argi : ci->top;

    if (p->exectarget != 0 && p->execdata)
        ci->flags |= LUA_CALLINFO_NATIVE;

    luau_execute(L);

    // resume_continue is not handled here as metamethods are not yieldable

    L->top = restorestack(L, funcoffset) + nresults;
    L->nCcalls--;
}

static StkId callTMres(lua_State* L, StkId res, const TValue* f, const TValue* p1, const TValue* p2)
{
    ptrdiff_t result = savestack(L, res);
    // using stack room beyond top is technically safe here, but for very complicated reasons:
    // * The stack guarantees EXTRA_STACK room beyond stack_last (see luaD_reallocstack) will be allocated
    // * we cannot move luaD_checkstack above because the arguments are *sometimes* pointers to the lua
    // stack and checkstack may invalidate those pointers
    // * we cannot use savestack/restorestack because the arguments are sometimes on the C++ stack
    // * during stack reallocation all of the allocated stack is copied (even beyond stack_last) so these
    // values will be preserved even if they go past stack_last
    if (FFlag::LuauCallLuauTm)
    {
        StkId top = L->top;
        LUAU_ASSERT((top + 3) < (L->stack + L->stacksize));
        setobj2s(L, top, f);      // push function
        setobj2s(L, top + 1, p1); // 1st argument
        setobj2s(L, top + 2, p2); // 2nd argument

        // fast-path for Luau to Luau calls
        if (L->isactive && isLua(L->ci) && ttisfunction(top) && !clvalue(top)->isC)
        {
            L->top += 3;
            callTMluau(L, top, clvalue(top), 1);
        }
        else
        {
            luaD_checkstack(L, 3);
            StkId func = L->top;
            L->top += 3;
            luaD_call(L, func, 1);
        }
    }
    else
    {
        LUAU_ASSERT((L->top + 3) < (L->stack + L->stacksize));
        setobj2s(L, L->top, f);      // push function
        setobj2s(L, L->top + 1, p1); // 1st argument
        setobj2s(L, L->top + 2, p2); // 2nd argument
        luaD_checkstack(L, 3);
        L->top += 3;
        luaD_call(L, L->top - 3, 1);
    }

    res = restorestack(L, result);
    L->top--;
    setobj2s(L, res, L->top);
    return res;
}

// There is only one call location for this function, but inlining it has been measured to have a negative effect on luaV_settable
static LUAU_NOINLINE void callTM(lua_State* L, const TValue* f, const TValue* p1, const TValue* p2, const TValue* p3)
{
    // using stack room beyond top is technically safe here, but for very complicated reasons:
    // * The stack guarantees EXTRA_STACK room beyond stack_last (see luaD_reallocstack) will be allocated
    // * we cannot move luaD_checkstack above because the arguments are *sometimes* pointers to the lua
    // stack and checkstack may invalidate those pointers
    // * we cannot use savestack/restorestack because the arguments are sometimes on the C++ stack
    // * during stack reallocation all of the allocated stack is copied (even beyond stack_last) so these
    // values will be preserved even if they go past stack_last
    if (FFlag::LuauCallLuauTm)
    {
        StkId top = L->top;
        LUAU_ASSERT((top + 4) < (L->stack + L->stacksize));
        setobj2s(L, top, f);      // push function
        setobj2s(L, top + 1, p1); // 1st argument
        setobj2s(L, top + 2, p2); // 2nd argument
        setobj2s(L, top + 3, p3); // 3th argument

        // fast-path for Luau to Luau calls
        if (L->isactive && isLua(L->ci) && ttisfunction(top) && !clvalue(top)->isC)
        {
            L->top += 4;
            callTMluau(L, top, clvalue(top), 0);
        }
        else
        {
            luaD_checkstack(L, 4);
            StkId func = L->top;
            L->top += 4;
            luaD_call(L, func, 0);
        }
    }
    else
    {
        LUAU_ASSERT((L->top + 4) < (L->stack + L->stacksize));
        setobj2s(L, L->top, f);      // push function
        setobj2s(L, L->top + 1, p1); // 1st argument
        setobj2s(L, L->top + 2, p2); // 2nd argument
        setobj2s(L, L->top + 3, p3); // 3th argument
        luaD_checkstack(L, 4);
        L->top += 4;
        luaD_call(L, L->top - 4, 0);
    }
}

void luaV_gettable(lua_State* L, const TValue* t, TValue* key, StkId val)
{
    int loop;
    for (loop = 0; loop < MAXTAGLOOP; loop++)
    {
        const TValue* tm;
        if (ttistable(t))
        { // `t' is a table?
            LuaTable* h = hvalue(t);

            const TValue* res = luaH_get(h, key); // do a primitive get

            if (res != luaO_nilobject)
                L->cachedslot = gval2slot(h, res); // remember slot to accelerate future lookups

            if (FFlag::LuauFrozenMetaButterfly)
            {
                if (LUAU_LIKELY(!ttisnil(res)))
                {
                    setobj2s(L, val, res);
                    return;
                }

                if ((tm = fasttm(L, h->metatable, TM_INDEX)) == NULL)
                {
                    setnilvalue(val);
                    return;
                }
            }
            else
            {
                if (!ttisnil(res) // result is no nil?
                    || (tm = fasttm(L, h->metatable, TM_INDEX)) == NULL)
                { // or no TM?
                    setobj2s(L, val, res);
                    return;
                }
            }

            // t isn't a table, so see if it has an INDEX meta-method to look up the key with
        }
        else if (LUAU_UNLIKELY(FFlag::DebugLuauUserDefinedClassesRuntime && ttisobject(t)))
        {
            LuauObject* inst = objectvalue(t);
            const TValue* offsettval = luaH_get(inst->lclass->memberstooffset, key);

            // Class instances throw if you try to access a member that is not
            // present.
            if (ttisnil(offsettval))
                luaG_missingmembererror(L, t, key);

            const uint32_t offset = uint32_t(nvalue(offsettval));
            setobj2s(L, val, luaR_lookupmemberatoffset(inst, offset));
            return;
        }
        else if (LUAU_UNLIKELY(FFlag::DebugLuauUserDefinedClassesRuntime && ttisclass(t)))
        {
            LuauClass* lco = classvalue(t);
            const TValue* res = luaH_get(lco->memberstooffset, key);

            // Class objects throw if you try to access a member that is not
            // present.
            if (ttisnil(res))
                luaG_missingmembererror(L, t, key);

            const uint32_t offset = uint32_t(nvalue(res));
            LUAU_ASSERT(offset < lco->numberofallmembers);

            // This is the case where we try to access an instance member on a
            // class object, for example:
            //
            //  class Box
            //      public item
            //      function print(self) print(self.item) end
            //  end
            //
            //  local _ = Box.item
            //
            if (offset < lco->numberofinstancemembers)
                luaG_missingmembererror(L, t, key);

            setobj2s(L, val, &lco->staticmembers[offset - lco->numberofinstancemembers]);
            return;
        }
        else if (ttisnil(tm = luaT_gettmbyobj(L, t, TM_INDEX)))
            luaG_indexerror(L, t, key);
        if (ttisfunction(tm))
        {
            callTMres(L, val, tm, t, key);
            return;
        }
        t = tm; // else repeat with `tm'
    }
    luaG_runerror(L, "'__index' chain too long; possible loop");
}

void luaV_settable(lua_State* L, const TValue* t, TValue* key, StkId val)
{
    int loop;
    TValue temp;
    for (loop = 0; loop < MAXTAGLOOP; loop++)
    {
        const TValue* tm;
        if (ttistable(t))
        { // `t' is a table?
            LuaTable* h = hvalue(t);

            const TValue* oldval = luaH_get(h, key);

            // should we assign the key? (if key is valid or __newindex is not set)
            if (!ttisnil(oldval) || (tm = fasttm(L, h->metatable, TM_NEWINDEX)) == NULL)
            {
                if (h->readonly)
                    luaG_readonlyerror(L);

                // luaH_set would work but would repeat the lookup so we use luaH_setslot that can reuse oldval if it's safe
                TValue* newval = luaH_setslot(L, h, oldval, key);

                L->cachedslot = gval2slot(h, newval); // remember slot to accelerate future lookups

                setobj2t(L, newval, val);
                luaC_barriert(L, h, val);
                return;
            }

            // fallthrough to metamethod
        }
        else if (LUAU_UNLIKELY(FFlag::DebugLuauUserDefinedClassesRuntime && ttisobject(t)))
        {
            LuauObject* inst = objectvalue(t);
            const TValue* offset = luaH_get(inst->lclass->memberstooffset, key);
            if (ttisnil(offset))
                luaG_missingmembererror(L, t, key);
            const uint32_t offsetnum = uint32_t(nvalue(offset));
            LUAU_ASSERT(offsetnum < inst->lclass->numberofallmembers);
            if (offsetnum >= inst->lclass->numberofinstancemembers)
                luaG_indexerror(L, t, key);
            setobj2class(L, &inst->members[offsetnum], val);
            luaC_barrier(L, inst, val);
            return;
        }
        else if (ttisnil(tm = luaT_gettmbyobj(L, t, TM_NEWINDEX)))
            luaG_indexerror(L, t, key);

        if (ttisfunction(tm))
        {
            callTM(L, tm, t, key, val);
            return;
        }
        // else repeat with `tm'
        setobj(L, &temp, tm); // avoid pointing inside table (may rehash)
        t = &temp;
    }
    luaG_runerror(L, "'__newindex' chain too long; possible loop");
}

static int call_binTM(lua_State* L, const TValue* p1, const TValue* p2, StkId res, TMS event)
{
    const TValue* tm = luaT_gettmbyobj(L, p1, event); // try first operand
    if (ttisnil(tm))
        tm = luaT_gettmbyobj(L, p2, event); // try second operand
    if (ttisnil(tm))
        return 0;
    callTMres(L, res, tm, p1, p2);
    return 1;
}

static const TValue* get_compTM(lua_State* L, LuaTable* mt1, LuaTable* mt2, TMS event)
{
    const TValue* tm1 = fasttm(L, mt1, event);
    const TValue* tm2;
    if (tm1 == NULL)
        return NULL; // no metamethod
    if (mt1 == mt2)
        return tm1; // same metatables => same metamethods
    tm2 = fasttm(L, mt2, event);
    if (tm2 == NULL)
        return NULL;                // no metamethod
    if (luaO_rawequalObj(tm1, tm2)) // same metamethods?
        return tm1;
    return NULL;
}

static int call_orderTM(lua_State* L, const TValue* p1, const TValue* p2, TMS event, bool error = false)
{
    const TValue* tm1 = luaT_gettmbyobj(L, p1, event);
    const TValue* tm2;
    if (ttisnil(tm1))
    {
        if (error)
            luaG_ordererror(L, p1, p2, event);
        return -1; // no metamethod?
    }
    tm2 = luaT_gettmbyobj(L, p2, event);
    if (!luaO_rawequalObj(tm1, tm2)) // different metamethods?
    {
        if (error)
            luaG_ordererror(L, p1, p2, event);
        return -1;
    }
    callTMres(L, L->top, tm1, p1, p2);
    return !l_isfalse(L->top);
}

int luaV_strcmp(const TString* ls, const TString* rs)
{
    if (ls == rs)
        return 0;

    const char* l = getstr(ls);
    const char* r = getstr(rs);

    // always safe to read one character because even empty strings are nul terminated
    if (*l != *r)
        return uint8_t(*l) - uint8_t(*r);

    size_t ll = ls->len;
    size_t lr = rs->len;
    size_t lmin = ll < lr ? ll : lr;

    int res = memcmp(l, r, lmin);
    if (res != 0)
        return res;

    return ll == lr ? 0 : ll < lr ? -1 : 1;
}

int luaV_lessthan(lua_State* L, const TValue* l, const TValue* r)
{
    if (LUAU_UNLIKELY(ttype(l) != ttype(r)))
        luaG_ordererror(L, l, r, TM_LT);
    else if (LUAU_LIKELY(ttisnumber(l)))
        return luai_numlt(nvalue(l), nvalue(r));
    else if (ttisstring(l))
        return luaV_strcmp(tsvalue(l), tsvalue(r)) < 0;
    else
        return call_orderTM(L, l, r, TM_LT, /* error= */ true);
}

int luaV_lessequal(lua_State* L, const TValue* l, const TValue* r)
{
    int res;
    if (ttype(l) != ttype(r))
        luaG_ordererror(L, l, r, TM_LE);
    else if (ttisnumber(l))
        return luai_numle(nvalue(l), nvalue(r));
    else if (ttisstring(l))
        return luaV_strcmp(tsvalue(l), tsvalue(r)) <= 0;
    else if ((res = call_orderTM(L, l, r, TM_LE)) != -1) // first try `le'
        return res;
    else if ((res = call_orderTM(L, r, l, TM_LT)) == -1) // error if not `lt'
        luaG_ordererror(L, l, r, TM_LE);
    return !res;
}

int luaV_equalval(lua_State* L, const TValue* t1, const TValue* t2)
{
    const TValue* tm;
    LUAU_ASSERT(ttype(t1) == ttype(t2));
    switch (ttype(t1))
    {
    case LUA_TNIL:
        return 1;
    case LUA_TNUMBER:
        return luai_numeq(nvalue(t1), nvalue(t2));
    case LUA_TINTEGER:
        return luai_inteq(lvalue(t1), lvalue(t2));
    case LUA_TVECTOR:
        return luai_veceq(vvalue(t1), vvalue(t2));
    case LUA_TBOOLEAN:
        return bvalue(t1) == bvalue(t2); // true must be 1 !!
    case LUA_TLIGHTUSERDATA:
        return pvalue(t1) == pvalue(t2) && lightuserdatatag(t1) == lightuserdatatag(t2);
    case LUA_TUSERDATA:
    {
        tm = get_compTM(L, uvalue(t1)->metatable, uvalue(t2)->metatable, TM_EQ);
        if (!tm)
            return uvalue(t1) == uvalue(t2);
        break; // will try TM
    }
    case LUA_TCLASS:
        return classvalue(t1) == classvalue(t2);
    case LUA_TOBJECT:
    {
        // We follow the same rules as metatables.
        tm = get_compTM(L, objectvalue(t1)->lclass->instancemetatable, objectvalue(t2)->lclass->instancemetatable, TM_EQ);
        if (!tm)
            return objectvalue(t1) == objectvalue(t2);
        break; // will try TM
    }
    case LUA_TTABLE:
    {
        tm = get_compTM(L, hvalue(t1)->metatable, hvalue(t2)->metatable, TM_EQ);
        if (!tm)
            return hvalue(t1) == hvalue(t2);
        break; // will try TM
    }
    default:
        return gcvalue(t1) == gcvalue(t2);
    }
    callTMres(L, L->top, tm, t1, t2); // call TM
    return !l_isfalse(L->top);
}

void luaV_concat(lua_State* L, int total, int last)
{
    do
    {
        StkId top = L->base + last + 1;
        int n = 2; // number of elements handled in this pass (at least 2)
        if (!(ttisstring(top - 2) || ttisnumber(top - 2)) || !tostring(L, top - 1))
        {
            if (!call_binTM(L, top - 2, top - 1, top - 2, TM_CONCAT))
                luaG_concaterror(L, top - 2, top - 1);
        }
        else if (tsvalue(top - 1)->len == 0) // second op is empty?
            (void)tostring(L, top - 2);      // result is first op (as string)
        else
        {
            // at least two string values; get as many as possible
            size_t tl = tsvalue(top - 1)->len;
            char* buffer;
            int i;
            // collect total length
            for (n = 1; n < total && tostring(L, top - n - 1); n++)
            {
                size_t l = tsvalue(top - n - 1)->len;
                if (l > MAXSSIZE - tl)
                    luaG_runerror(L, "string length overflow");
                tl += l;
            }

            char buf[LUA_BUFFERSIZE];
            TString* ts = nullptr;

            if (tl < LUA_BUFFERSIZE)
            {
                buffer = buf;
            }
            else
            {
                ts = luaS_bufstart(L, tl);
                buffer = ts->data;
            }

            tl = 0;
            for (i = n; i > 0; i--)
            { // concat all strings
                size_t l = tsvalue(top - i)->len;
                memcpy(buffer + tl, svalue(top - i), l);
                tl += l;
            }

            if (tl < LUA_BUFFERSIZE)
            {
                setsvalue(L, top - n, luaS_newlstr(L, buffer, tl));
            }
            else
            {
                setsvalue(L, top - n, luaS_buffinish(L, ts));
            }
        }
        total -= n - 1; // got `n' strings to create 1 new
        last -= n - 1;
    } while (total > 1); // repeat until only 1 result left
}

template<TMS op>
void luaV_doarithimpl(lua_State* L, StkId ra, const TValue* rb, const TValue* rc)
{
    TValue tempb, tempc;
    const TValue *b, *c;

    // vector operations that we support:
    // v+v  v-v  -v    (add/sub/neg)
    // v*v  s*v  v*s   (mul)
    // v/v  s/v  v/s   (div)
    // v//v s//v v//s  (floor div)
    const LUA_VECTOR_TYPE* vb = ttisvector(rb) ? vvalue(rb) : nullptr;
    const LUA_VECTOR_TYPE* vc = ttisvector(rc) ? vvalue(rc) : nullptr;

    if (vb && vc)
    {
        switch (op)
        {
        case TM_ADD:
            setvvalue(L, ra, vb[0] + vc[0], vb[1] + vc[1], vb[2] + vc[2], vb[3] + vc[3]);
            return;
        case TM_SUB:
            setvvalue(L, ra, vb[0] - vc[0], vb[1] - vc[1], vb[2] - vc[2], vb[3] - vc[3]);
            return;
        case TM_MUL:
            setvvalue(L, ra, vb[0] * vc[0], vb[1] * vc[1], vb[2] * vc[2], vb[3] * vc[3]);
            return;
        case TM_DIV:
            setvvalue(L, ra, vb[0] / vc[0], vb[1] / vc[1], vb[2] / vc[2], vb[3] / vc[3]);
            return;
        case TM_IDIV:
            setvvalue(
                L,
                ra,
                LUA_VECTOR_TYPE(luai_numidiv(vb[0], vc[0])),
                LUA_VECTOR_TYPE(luai_numidiv(vb[1], vc[1])),
                LUA_VECTOR_TYPE(luai_numidiv(vb[2], vc[2])),
                LUA_VECTOR_TYPE(luai_numidiv(vb[3], vc[3]))
            );
            return;
        case TM_UNM:
            setvvalue(L, ra, -vb[0], -vb[1], -vb[2], -vb[3]);
            return;
        default:
            break;
        }
    }
    else if (vb)
    {
        c = ttisnumber(rc) ? rc : luaV_tonumber(rc, &tempc);

        if (c)
        {
            LUA_VECTOR_TYPE nc = cast_to(LUA_VECTOR_TYPE, nvalue(c));

            switch (op)
            {
            case TM_MUL:
                setvvalue(L, ra, vb[0] * nc, vb[1] * nc, vb[2] * nc, vb[3] * nc);
                return;
            case TM_DIV:
                setvvalue(L, ra, vb[0] / nc, vb[1] / nc, vb[2] / nc, vb[3] / nc);
                return;
            case TM_IDIV:
                setvvalue(
                    L,
                    ra,
                    LUA_VECTOR_TYPE(luai_numidiv(vb[0], nc)),
                    LUA_VECTOR_TYPE(luai_numidiv(vb[1], nc)),
                    LUA_VECTOR_TYPE(luai_numidiv(vb[2], nc)),
                    LUA_VECTOR_TYPE(luai_numidiv(vb[3], nc))
                );
                return;
            default:
                break;
            }
        }
    }
    else if (vc)
    {
        b = ttisnumber(rb) ? rb : luaV_tonumber(rb, &tempb);

        if (b)
        {
            LUA_VECTOR_TYPE nb = cast_to(LUA_VECTOR_TYPE, nvalue(b));

            switch (op)
            {
            case TM_MUL:
                setvvalue(L, ra, nb * vc[0], nb * vc[1], nb * vc[2], nb * vc[3]);
                return;
            case TM_DIV:
                setvvalue(L, ra, nb / vc[0], nb / vc[1], nb / vc[2], nb / vc[3]);
                return;
            case TM_IDIV:
                setvvalue(
                    L,
                    ra,
                    LUA_VECTOR_TYPE(luai_numidiv(nb, vc[0])),
                    LUA_VECTOR_TYPE(luai_numidiv(nb, vc[1])),
                    LUA_VECTOR_TYPE(luai_numidiv(nb, vc[2])),
                    LUA_VECTOR_TYPE(luai_numidiv(nb, vc[3]))
                );
                return;
            default:
                break;
            }
        }
    }

    if ((b = luaV_tonumber(rb, &tempb)) != NULL && (c = luaV_tonumber(rc, &tempc)) != NULL)
    {
        double nb = nvalue(b), nc = nvalue(c);

        switch (op)
        {
        case TM_ADD:
            setnvalue(ra, luai_numadd(nb, nc));
            break;
        case TM_SUB:
            setnvalue(ra, luai_numsub(nb, nc));
            break;
        case TM_MUL:
            setnvalue(ra, luai_nummul(nb, nc));
            break;
        case TM_DIV:
            setnvalue(ra, luai_numdiv(nb, nc));
            break;
        case TM_IDIV:
            setnvalue(ra, luai_numidiv(nb, nc));
            break;
        case TM_MOD:
            setnvalue(ra, luai_nummod(nb, nc));
            break;
        case TM_POW:
            setnvalue(ra, luai_numpow(nb, nc));
            break;
        case TM_UNM:
            setnvalue(ra, luai_numunm(nb));
            break;
        default:
            LUAU_ASSERT(0);
            break;
        }
    }
    else
    {
        if (!call_binTM(L, rb, rc, ra, op))
        {
            luaG_aritherror(L, rb, rc, op);
        }
    }
}

// instantiate private template implementation for external callers
template void luaV_doarithimpl<TM_ADD>(lua_State* L, StkId ra, const TValue* rb, const TValue* rc);
template void luaV_doarithimpl<TM_SUB>(lua_State* L, StkId ra, const TValue* rb, const TValue* rc);
template void luaV_doarithimpl<TM_MUL>(lua_State* L, StkId ra, const TValue* rb, const TValue* rc);
template void luaV_doarithimpl<TM_DIV>(lua_State* L, StkId ra, const TValue* rb, const TValue* rc);
template void luaV_doarithimpl<TM_IDIV>(lua_State* L, StkId ra, const TValue* rb, const TValue* rc);
template void luaV_doarithimpl<TM_MOD>(lua_State* L, StkId ra, const TValue* rb, const TValue* rc);
template void luaV_doarithimpl<TM_POW>(lua_State* L, StkId ra, const TValue* rb, const TValue* rc);
template void luaV_doarithimpl<TM_UNM>(lua_State* L, StkId ra, const TValue* rb, const TValue* rc);

void luaV_dolen(lua_State* L, StkId ra, const TValue* rb)
{
    const TValue* tm = NULL;
    switch (ttype(rb))
    {
    case LUA_TTABLE:
    {
        LuaTable* h = hvalue(rb);
        if ((tm = fasttm(L, h->metatable, TM_LEN)) == NULL)
        {
            setnvalue(ra, cast_num(luaH_getn(h)));
            return;
        }
        break;
    }
    case LUA_TSTRING:
    {
        TString* ts = tsvalue(rb);
        setnvalue(ra, cast_num(ts->len));
        return;
    }
    default:
        tm = luaT_gettmbyobj(L, rb, TM_LEN);
    }

    if (ttisnil(tm))
        luaG_typeerror(L, rb, "get length of");

    StkId res = callTMres(L, ra, tm, rb, luaO_nilobject);
    if (!ttisnumber(res))
        luaG_runerror(L, "'__len' must return a number"); // note, we can't access rb since stack may have been reallocated
}

LUAU_NOINLINE void luaV_prepareFORN(lua_State* L, StkId plimit, StkId pstep, StkId pinit)
{
    if (!ttisnumber(pinit) && !luaV_tonumber(pinit, pinit))
        luaG_forerror(L, pinit, "initial value");
    if (!ttisnumber(plimit) && !luaV_tonumber(plimit, plimit))
        luaG_forerror(L, plimit, "limit");
    if (!ttisnumber(pstep) && !luaV_tonumber(pstep, pstep))
        luaG_forerror(L, pstep, "step");
}

// calls a C function f with no yielding support; optionally save one resulting value to the res register
// the function and arguments have to already be pushed to L->top
LUAU_NOINLINE void luaV_callTM(lua_State* L, int nparams, int res)
{
    ++L->nCcalls;

    if (L->nCcalls >= LUAI_MAXCCALLS)
        luaD_checkCstack(L);

    luaD_checkstack(L, LUA_MINSTACK);

    StkId top = L->top;
    StkId fun = top - nparams - 1;

    CallInfo* ci = incr_ci(L);
    ci->func = fun;
    ci->p = nullptr;
    ci->base = fun + 1;
    ci->top = top + LUA_MINSTACK;
    ci->savedpc = NULL;
    ci->flags = 0;
    ci->nresults = (res >= 0);
    LUAU_ASSERT(ci->top <= L->stack_last);

    LUAU_ASSERT(ttisfunction(ci->func));
    LUAU_ASSERT(clvalue(ci->func)->isC);

    L->base = fun + 1;
    LUAU_ASSERT(L->top == L->base + nparams);

    lua_CFunction func = clvalue(fun)->c.f;
    int n = func(L);
    LUAU_ASSERT(n >= 0); // yields should have been blocked by nCcalls

    // ci is our callinfo, cip is our parent
    // note that we read L->ci again since it may have been reallocated by the call
    CallInfo* cip = L->ci - 1;

    // copy return value into parent stack
    if (res >= 0)
    {
        if (n > 0)
        {
            setobj2s(L, &cip->base[res], L->top - n);
        }
        else
        {
            setnilvalue(&cip->base[res]);
        }
    }

    L->ci = cip;
    L->base = cip->base;
    L->top = cip->top;

    --L->nCcalls;
}

LUAU_NOINLINE void luaV_tryfuncTM(lua_State* L, StkId func)
{
    const TValue* tm = luaT_gettmbyobj(L, func, TM_CALL);
    if (!ttisfunction(tm))
        luaG_typeerror(L, func, "call");
    for (StkId p = L->top; p > func; p--) // open space for metamethod
        setobj2s(L, p, p - 1);
    L->top++;              // stack space pre-allocated by the caller
    setobj2s(L, func, tm); // tag method is the new function to be called
}
