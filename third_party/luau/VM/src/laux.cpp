// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "lualib.h"

#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "lapi.h"
#include "lgc.h"
#include "lnumutils.h"

#include <string.h>

// convert a stack index to positive
#define abs_index(L, i) ((i) > 0 || (i) <= LUA_REGISTRYINDEX ? (i) : lua_gettop(L) + (i) + 1)

/*
** {======================================================
** Error-report functions
** =======================================================
*/

static const char* currfuncname(lua_State* L)
{
    Closure* cl = L->ci > L->base_ci ? curr_func(L) : NULL;

    const char* debugname = cl && cl->isC && cl->c.debugname ? getstr(cl->c.debugname) : NULL;

    if (debugname && strcmp(debugname, "__namecall") == 0)
        return L->namecall ? getstr(L->namecall) : NULL;
    else
        return debugname;
}

l_noret luaL_argerrorL(lua_State* L, int narg, const char* extramsg)
{
    const char* fname = currfuncname(L);

    if (fname)
        luaL_error(L, "invalid argument #%d to '%s' (%s)", narg, fname, extramsg);
    else
        luaL_error(L, "invalid argument #%d (%s)", narg, extramsg);
}

l_noret luaL_typeerrorL(lua_State* L, int narg, const char* tname)
{
    const char* fname = currfuncname(L);
    const TValue* obj = luaA_toobject(L, narg);

    if (obj)
    {
        if (fname)
            luaL_error(L, "invalid argument #%d to '%s' (%s expected, got %s)", narg, fname, tname, luaT_objtypename(L, obj));
        else
            luaL_error(L, "invalid argument #%d (%s expected, got %s)", narg, tname, luaT_objtypename(L, obj));
    }
    else
    {
        if (fname)
            luaL_error(L, "missing argument #%d to '%s' (%s expected)", narg, fname, tname);
        else
            luaL_error(L, "missing argument #%d (%s expected)", narg, tname);
    }
}

static l_noret tag_error(lua_State* L, int narg, int tag)
{
    luaL_typeerrorL(L, narg, lua_typename(L, tag));
}

// Can be called without stack space reservation
void luaL_where(lua_State* L, int level)
{
    lua_Debug ar;
    if (lua_getinfo(L, level, "sl", &ar) && ar.currentline > 0)
    {
        lua_pushfstring(L, "%s:%d: ", ar.short_src, ar.currentline);
        return;
    }

    lua_rawcheckstack(L, 1);
    lua_pushliteral(L, ""); // else, no information available...
}

// Can be called without stack space reservation
l_noret luaL_errorL(lua_State* L, const char* fmt, ...)
{
    va_list argp;
    va_start(argp, fmt);
    luaL_where(L, 1);
    lua_pushvfstring(L, fmt, argp);
    va_end(argp);
    lua_concat(L, 2);
    lua_error(L);
}

// }======================================================

int luaL_checkoption(lua_State* L, int narg, const char* def, const char* const lst[])
{
    const char* name = (def) ? luaL_optstring(L, narg, def) : luaL_checkstring(L, narg);
    int i;
    for (i = 0; lst[i]; i++)
        if (strcmp(lst[i], name) == 0)
            return i;
    const char* msg = lua_pushfstring(L, "invalid option '%s'", name);
    luaL_argerrorL(L, narg, msg);
}

int luaL_newmetatable(lua_State* L, const char* tname)
{
    lua_getfield(L, LUA_REGISTRYINDEX, tname); // get registry.name
    if (!lua_isnil(L, -1))                     // name already in use?
        return 0;                              // leave previous value on top, but return 0
    lua_pop(L, 1);
    lua_newtable(L); // create metatable
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, tname); // registry.name = metatable
    return 1;
}

void* luaL_checkudata(lua_State* L, int ud, const char* tname)
{
    void* p = lua_touserdata(L, ud);
    if (p != NULL)
    { // value is a userdata?
        if (lua_getmetatable(L, ud))
        {                                              // does it have a metatable?
            lua_getfield(L, LUA_REGISTRYINDEX, tname); // get correct metatable
            if (lua_rawequal(L, -1, -2))
            {                  // does it have the correct mt?
                lua_pop(L, 2); // remove both metatables
                return p;
            }
        }
    }
    luaL_typeerrorL(L, ud, tname); // else error
}

void* luaL_checkudatatagged(lua_State* L, int ud, int tag)
{
    void* p = lua_touserdatatagged(L, ud, tag);
    if (p != NULL)
        return p;

    const char* tname = lua_getuserdataname(L, tag);
    luaL_typeerrorL(L, ud, tname); // else error
}

void* luaL_checkbuffer(lua_State* L, int narg, size_t* len)
{
    void* b = lua_tobuffer(L, narg, len);
    if (!b)
        tag_error(L, narg, LUA_TBUFFER);
    return b;
}

void luaL_checkstack(lua_State* L, int space, const char* mes)
{
    if (!lua_checkstack(L, space))
        luaL_error(L, "stack overflow (%s)", mes);
}

void luaL_checktype(lua_State* L, int narg, int t)
{
    if (lua_type(L, narg) != t)
        tag_error(L, narg, t);
}

void luaL_checkany(lua_State* L, int narg)
{
    if (lua_type(L, narg) == LUA_TNONE)
        luaL_error(L, "missing argument #%d", narg);
}

const char* luaL_checklstring(lua_State* L, int narg, size_t* len)
{
    const char* s = lua_tolstring(L, narg, len);
    if (!s)
        tag_error(L, narg, LUA_TSTRING);
    return s;
}

const char* luaL_optlstring(lua_State* L, int narg, const char* def, size_t* len)
{
    if (lua_isnoneornil(L, narg))
    {
        if (len)
            *len = (def ? strlen(def) : 0);
        return def;
    }
    else
        return luaL_checklstring(L, narg, len);
}

double luaL_checknumber(lua_State* L, int narg)
{
    int isnum;
    double d = lua_tonumberx(L, narg, &isnum);
    if (!isnum)
        tag_error(L, narg, LUA_TNUMBER);
    return d;
}

double luaL_optnumber(lua_State* L, int narg, double def)
{
    return luaL_opt(L, luaL_checknumber, narg, def);
}

int luaL_checkboolean(lua_State* L, int narg)
{
    // This checks specifically for boolean values, ignoring
    // all other truthy/falsy values. If the desired result
    // is true if value is present then lua_toboolean should
    // directly be used instead.
    if (!lua_isboolean(L, narg))
        tag_error(L, narg, LUA_TBOOLEAN);
    return lua_toboolean(L, narg);
}

int luaL_optboolean(lua_State* L, int narg, int def)
{
    return luaL_opt(L, luaL_checkboolean, narg, def);
}

int luaL_checkinteger(lua_State* L, int narg)
{
    int isnum;
    int d = lua_tointegerx(L, narg, &isnum);
    if (!isnum)
        tag_error(L, narg, LUA_TNUMBER);
    return d;
}

int64_t luaL_checkinteger64(lua_State* L, int narg)
{
    if (!lua_isinteger64(L, narg))
        tag_error(L, narg, LUA_TINTEGER);
    return lua_tointeger64(L, narg, nullptr);
}

int luaL_optinteger(lua_State* L, int narg, int def)
{
    return luaL_opt(L, luaL_checkinteger, narg, def);
}

int64_t luaL_optinteger64(lua_State* L, int narg, int64_t def)
{
    return luaL_opt(L, luaL_checkinteger64, narg, def);
}

unsigned luaL_checkunsigned(lua_State* L, int narg)
{
    int isnum;
    unsigned d = lua_tounsignedx(L, narg, &isnum);
    if (!isnum)
        tag_error(L, narg, LUA_TNUMBER);
    return d;
}

unsigned luaL_optunsigned(lua_State* L, int narg, unsigned def)
{
    return luaL_opt(L, luaL_checkunsigned, narg, def);
}

const LUA_VECTOR_TYPE* luaL_checkvector(lua_State* L, int narg)
{
    const LUA_VECTOR_TYPE* v = lua_tovector(L, narg);
    if (!v)
        tag_error(L, narg, LUA_TVECTOR);
    return v;
}

const LUA_VECTOR_TYPE* luaL_optvector(lua_State* L, int narg, const LUA_VECTOR_TYPE* def)
{
    return luaL_opt(L, luaL_checkvector, narg, def);
}

int luaL_getmetafield(lua_State* L, int obj, const char* event)
{
    if (!lua_getmetatable(L, obj)) // no metatable?
        return 0;
    lua_pushstring(L, event);
    lua_rawget(L, -2);
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 2); // remove metatable and metafield
        return 0;
    }
    else
    {
        lua_remove(L, -2); // remove only metatable
        return 1;
    }
}

int luaL_callmeta(lua_State* L, int obj, const char* event)
{
    obj = abs_index(L, obj);
    if (!luaL_getmetafield(L, obj, event)) // no metafield?
        return 0;
    lua_pushvalue(L, obj);
    lua_call(L, 1, 1);
    return 1;
}

static int libsize(const luaL_Reg* l)
{
    int size = 0;
    for (; l->name; l++)
        size++;
    return size;
}

void luaL_register(lua_State* L, const char* libname, const luaL_Reg* l)
{
    if (libname)
    {
        int size = libsize(l);
        // check whether lib already exists
        luaL_findtable(L, LUA_REGISTRYINDEX, "_LOADED", 1);
        lua_getfield(L, -1, libname); // get _LOADED[libname]
        if (!lua_istable(L, -1))
        {                  // not found?
            lua_pop(L, 1); // remove previous result
            // try global variable (and create one if it does not exist)
            if (luaL_findtable(L, LUA_GLOBALSINDEX, libname, size) != NULL)
                luaL_error(L, "name conflict for module '%s'", libname);
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, libname); // _LOADED[libname] = new table
        }
        lua_remove(L, -2); // remove _LOADED table
    }
    for (; l->name; l++)
    {
        lua_pushcfunction(L, l->func, l->name);
        lua_setfield(L, -2, l->name);
    }
}

const char* luaL_findtable(lua_State* L, int idx, const char* fname, int szhint)
{
    const char* e;
    lua_pushvalue(L, idx);
    do
    {
        e = strchr(fname, '.');
        if (e == NULL)
            e = fname + strlen(fname);
        lua_pushlstring(L, fname, e - fname);
        lua_rawget(L, -2);
        if (lua_isnil(L, -1))
        {                                                    // no such field?
            lua_pop(L, 1);                                   // remove this nil
            lua_createtable(L, 0, (*e == '.' ? 1 : szhint)); // new table for field
            lua_pushlstring(L, fname, e - fname);
            lua_pushvalue(L, -2);
            lua_settable(L, -4); // set new table into field
        }
        else if (!lua_istable(L, -1))
        {                  // field has a non-table value?
            lua_pop(L, 2); // remove table and value
            return fname;  // return problematic part of the name
        }
        lua_remove(L, -2); // remove previous table
        fname = e + 1;
    } while (*e == '.');
    return NULL;
}

const char* luaL_typename(lua_State* L, int idx)
{
    const TValue* obj = luaA_toobject(L, idx);
    return obj ? luaT_objtypename(L, obj) : "no value";
}

void luaL_traceback(lua_State* L, lua_State* L1, const char* msg, int level)
{
    api_check(L, level >= 0);

    luaL_Strbuf buf;
    luaL_buffinit(L, &buf);

    if (msg)
    {
        luaL_addstring(&buf, msg);
        luaL_addstring(&buf, "\n");
    }

    lua_Debug ar;
    for (int i = level; lua_getinfo(L1, i, "sln", &ar); ++i)
    {
        if (strcmp(ar.what, "C") == 0)
            continue;

        if (ar.source)
            luaL_addstring(&buf, ar.short_src);

        if (ar.currentline > 0)
        {
            char line[32]; // manual conversion for performance
            char* lineend = line + sizeof(line);
            char* lineptr = lineend;
            for (unsigned int r = ar.currentline; r > 0; r /= 10)
                *--lineptr = '0' + (r % 10);

            luaL_addchar(&buf, ':');
            luaL_addlstring(&buf, lineptr, lineend - lineptr);
        }

        if (ar.name)
        {
            luaL_addstring(&buf, " function ");
            luaL_addstring(&buf, ar.name);
        }

        luaL_addchar(&buf, '\n');
    }

    luaL_pushresult(&buf);
}


/*
** {======================================================
** Generic Buffer manipulation
** =======================================================
*/

static size_t getnextbuffersize(lua_State* L, size_t currentsize, size_t desiredsize)
{
    size_t newsize = currentsize + currentsize / 2;

    // check for size overflow
    if (SIZE_MAX - desiredsize < currentsize)
        luaL_error(L, "buffer too large");

    // growth factor might not be enough to satisfy the desired size
    if (newsize < desiredsize)
        newsize = desiredsize;

    return newsize;
}

static char* extendstrbuf(luaL_Strbuf* B, size_t additionalsize, int boxloc)
{
    lua_State* L = B->L;

    if (B->storage)
        LUAU_ASSERT(B->storage == tsvalue(L->top + boxloc));

    char* base = B->storage ? B->storage->data : B->buffer;

    size_t capacity = B->end - base;
    size_t nextsize = getnextbuffersize(B->L, capacity, capacity + additionalsize);

    TString* newStorage = luaS_bufstart(L, nextsize);

    memcpy(newStorage->data, base, B->p - base);

    // place the string storage at the expected position in the stack
    if (base == B->buffer)
    {
        lua_pushnil(L);
        lua_insert(L, boxloc);
    }

    setsvalue(L, L->top + boxloc, newStorage);
    B->p = newStorage->data + (B->p - base);
    B->end = newStorage->data + nextsize;
    B->storage = newStorage;

    return B->p;
}

void luaL_buffinit(lua_State* L, luaL_Strbuf* B)
{
    // start with an internal buffer
    B->p = B->buffer;
    B->end = B->p + LUA_BUFFERSIZE;

    B->L = L;
    B->storage = nullptr;
}

char* luaL_buffinitsize(lua_State* L, luaL_Strbuf* B, size_t size)
{
    luaL_buffinit(L, B);
    return luaL_prepbuffsize(B, size);
}

char* luaL_prepbuffsize(luaL_Strbuf* B, size_t size)
{
    if (size_t(B->end - B->p) < size)
        return extendstrbuf(B, size - (B->end - B->p), -1);
    return B->p;
}

void luaL_addlstring(luaL_Strbuf* B, const char* s, size_t len)
{
    if (size_t(B->end - B->p) < len)
        extendstrbuf(B, len - (B->end - B->p), -1);

    memcpy(B->p, s, len);
    B->p += len;
}

void luaL_addvalue(luaL_Strbuf* B)
{
    lua_State* L = B->L;

    size_t vl;
    if (const char* s = lua_tolstring(L, -1, &vl))
    {
        if (size_t(B->end - B->p) < vl)
            extendstrbuf(B, vl - (B->end - B->p), -2);

        memcpy(B->p, s, vl);
        B->p += vl;

        lua_pop(L, 1);
    }
}

void luaL_addvalueany(luaL_Strbuf* B, int idx)
{
    lua_State* L = B->L;

    switch (lua_type(L, idx))
    {
    case LUA_TNONE:
    {
        LUAU_ASSERT(!"expected value");
        break;
    }
    case LUA_TNIL:
        luaL_addstring(B, "nil");
        break;
    case LUA_TBOOLEAN:
        if (lua_toboolean(L, idx))
            luaL_addstring(B, "true");
        else
            luaL_addstring(B, "false");
        break;
    case LUA_TNUMBER:
    {
        double n = lua_tonumber(L, idx);
        char s[LUAI_MAXNUM2STR];
        char* e = luai_num2str(s, n);
        luaL_addlstring(B, s, e - s);
        break;
    }
    case LUA_TSTRING:
    {
        size_t len;
        const char* s = lua_tolstring(L, idx, &len);
        luaL_addlstring(B, s, len);
        break;
    }
    case LUA_TINTEGER:
    {
        int64_t n = lua_tointeger64(L, idx, nullptr);
        char s[LUAI_MAXINT2STR];
        char* e = luai_int2str(s, n);
        luaL_addlstring(B, s, e - s);
        break;
    }
    default:
    {
        size_t len;
        luaL_tolstring(L, idx, &len);

        // note: luaL_addlstring assumes box is stored at top of stack, so we can't call it here
        // instead we use luaL_addvalue which will take the string from the top of the stack and add that
        luaL_addvalue(B);
    }
    }
}

void luaL_pushresult(luaL_Strbuf* B)
{
    lua_State* L = B->L;

    if (TString* storage = B->storage)
    {
        luaC_checkGC(L);

        // if we finished just at the end of the string buffer, we can convert it to a mutable string without a copy
        if (B->p == B->end)
        {
            setsvalue(L, L->top - 1, luaS_buffinish(L, storage));
        }
        else
        {
            setsvalue(L, L->top - 1, luaS_newlstr(L, storage->data, B->p - storage->data));
        }
    }
    else
    {
        lua_pushlstring(L, B->buffer, B->p - B->buffer);
    }
}

void luaL_pushresultsize(luaL_Strbuf* B, size_t size)
{
    B->p += size;
    luaL_pushresult(B);
}

// }======================================================

const char* luaL_tolstring(lua_State* L, int idx, size_t* len)
{
    if (luaL_callmeta(L, idx, "__tostring")) // is there a metafield?
    {
        const char* s = lua_tolstring(L, -1, len);
        if (!s)
            luaL_error(L, "'__tostring' must return a string");
        return s;
    }

    switch (lua_type(L, idx))
    {
    case LUA_TNIL:
        lua_pushliteral(L, "nil");
        break;
    case LUA_TBOOLEAN:
        lua_pushstring(L, (lua_toboolean(L, idx) ? "true" : "false"));
        break;
    case LUA_TNUMBER:
    {
        double n = lua_tonumber(L, idx);
        char s[LUAI_MAXNUM2STR];
        char* e = luai_num2str(s, n);
        lua_pushlstring(L, s, e - s);
        break;
    }
    case LUA_TVECTOR:
    {
        const LUA_VECTOR_TYPE* v = lua_tovector(L, idx);

        char s[LUAI_MAXNUM2STR * LUA_VECTOR_SIZE];
        char* e = s;
        for (int i = 0; i < LUA_VECTOR_SIZE; ++i)
        {
            if (i != 0)
            {
                *e++ = ',';
                *e++ = ' ';
            }
            e = luai_num2str(e, v[i]);
        }
        lua_pushlstring(L, s, e - s);
        break;
    }
    case LUA_TSTRING:
        lua_pushvalue(L, idx);
        break;
    case LUA_TINTEGER:
    {
        int64_t l = lua_tointeger64(L, idx, nullptr);
        char s[LUAI_MAXINT2STR];
        char* e = luai_int2str(s, l);
        lua_pushlstring(L, s, e - s);
        break;
    }
    default:
    {
        const void* ptr = lua_topointer(L, idx);
        unsigned long long enc = lua_encodepointer(L, uintptr_t(ptr));
        lua_pushfstring(L, "%s: 0x%016llx", luaL_typename(L, idx), enc);
        break;
    }
    }
    return lua_tolstring(L, -1, len);
}
