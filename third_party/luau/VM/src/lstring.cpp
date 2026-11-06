// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "lstring.h"

#include "lgc.h"
#include "lmem.h"

#include <string.h>

unsigned int luaS_hash(const char* str, size_t len)
{
    // Note that this hashing algorithm is replicated in BytecodeBuilder.cpp, BytecodeBuilder::getStringHash
    unsigned int a = 0, b = 0;
    unsigned int h = unsigned(len);

    // hash prefix in 12b chunks (using aligned reads) with ARX based hash (LuaJIT v2.1, lookup3)
    // note that we stop at length<32 to maintain compatibility with Lua 5.1
    while (len >= 32)
    {
#define rol(x, s) ((x >> s) | (x << (32 - s)))
#define mix(u, v, w) a ^= h, a -= rol(h, u), b ^= a, b -= rol(a, v), h ^= b, h -= rol(b, w)

        // should compile into fast unaligned reads
        uint32_t block[3];
        memcpy(block, str, 12);

        a += block[0];
        b += block[1];
        h += block[2];
        mix(14, 11, 25);
        str += 12;
        len -= 12;

#undef mix
#undef rol
    }

    // original Lua 5.1 hash for compatibility (exact match when len<32)
    for (size_t i = len; i > 0; --i)
        h ^= (h << 5) + (h >> 2) + (uint8_t)str[i - 1];

    return h;
}

void luaS_resize(lua_State* L, int newsize)
{
    TString** newhash = luaM_newarray(L, newsize, TString*, 0);
    stringtable* tb = &L->global->strt;
    for (int i = 0; i < newsize; i++)
        newhash[i] = NULL;
    // rehash
    for (int i = 0; i < tb->size; i++)
    {
        TString* p = tb->hash[i];
        while (p)
        {                            // for each node in the list
            TString* next = p->next; // save next
            unsigned int h = p->hash;
            int h1 = lmod(h, newsize); // new position
            LUAU_ASSERT(cast_int(h % newsize) == lmod(h, newsize));
            p->next = newhash[h1]; // chain it
            newhash[h1] = p;
            p = next;
        }
    }
    luaM_freearray(L, tb->hash, tb->size, TString*, 0);
    tb->size = newsize;
    tb->hash = newhash;
}

static TString* newlstr(lua_State* L, const char* str, size_t l, unsigned int h)
{
    if (l > MAXSSIZE)
        luaM_toobig(L);

    TString* ts = luaM_newgco(L, TString, sizestring(l), L->activememcat, LUA_TSTRING);
    luaC_init(L, ts, LUA_TSTRING);
    ts->atom = ATOM_UNDEF;
    ts->hash = h;
    ts->len = unsigned(l);

    memcpy(ts->data, str, l);
    ts->data[l] = '\0'; // ending 0

    stringtable* tb = &L->global->strt;
    h = lmod(h, tb->size);
    ts->next = tb->hash[h]; // chain new entry
    tb->hash[h] = ts;

    tb->nuse++;
    if (tb->nuse > cast_to(uint32_t, tb->size) && tb->size <= INT_MAX / 2)
        luaS_resize(L, tb->size * 2); // too crowded

    return ts;
}

TString* luaS_bufstart(lua_State* L, size_t size)
{
    if (size > MAXSSIZE)
        luaM_toobig(L);

    TString* ts = luaM_newgco(L, TString, sizestring(size), L->activememcat, LUA_TSTRING);
    luaC_init(L, ts, LUA_TSTRING);
    ts->atom = ATOM_UNDEF;
    ts->hash = 0; // computed in luaS_buffinish
    ts->len = unsigned(size);

    ts->next = NULL;

    return ts;
}

TString* luaS_buffinish(lua_State* L, TString* ts)
{
    unsigned int h = luaS_hash(ts->data, ts->len);
    stringtable* tb = &L->global->strt;
    int bucket = lmod(h, tb->size);

    // search if we already have this string in the hash table
    for (TString* el = tb->hash[bucket]; el != NULL; el = el->next)
    {
        if (el->len == ts->len && memcmp(el->data, ts->data, ts->len) == 0)
        {
            // string may be dead
            if (isdead(L->global, obj2gco(el)))
                changewhite(obj2gco(el));

            return el;
        }
    }

    LUAU_ASSERT(ts->next == NULL);

    ts->hash = h;
    ts->data[ts->len] = '\0'; // ending 0
    ts->atom = ATOM_UNDEF;
    ts->next = tb->hash[bucket]; // chain new entry
    tb->hash[bucket] = ts;

    tb->nuse++;
    if (tb->nuse > cast_to(uint32_t, tb->size) && tb->size <= INT_MAX / 2)
        luaS_resize(L, tb->size * 2); // too crowded

    return ts;
}

TString* luaS_newlstr(lua_State* L, const char* str, size_t l)
{
    unsigned int h = luaS_hash(str, l);
    for (TString* el = L->global->strt.hash[lmod(h, L->global->strt.size)]; el != NULL; el = el->next)
    {
        if (el->len == l && (memcmp(str, getstr(el), l) == 0))
        {
            // string may be dead
            if (isdead(L->global, obj2gco(el)))
                changewhite(obj2gco(el));
            return el;
        }
    }
    return newlstr(L, str, l, h); // not found
}

static bool unlinkstr(lua_State* L, TString* ts)
{
    global_State* g = L->global;

    TString** p = &g->strt.hash[lmod(ts->hash, g->strt.size)];

    while (TString* curr = *p)
    {
        if (curr == ts)
        {
            *p = curr->next;
            return true;
        }
        else
        {
            p = &curr->next;
        }
    }

    return false;
}

void luaS_free(lua_State* L, TString* ts, lua_Page* page)
{
    if (unlinkstr(L, ts))
        L->global->strt.nuse--;
    else
        LUAU_ASSERT(ts->next == NULL); // orphaned string buffer

    luaM_freegco(L, ts, sizestring(ts->len), ts->memcat, page);
}
