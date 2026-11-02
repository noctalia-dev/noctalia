// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details

/*
 * Implementation of tables (aka arrays, objects, or hash tables).
 *
 * Tables keep the elements in two parts: an array part and a hash part.
 * Integer keys >=1 are all candidates to be kept in the array part. The actual size of the array is the
 * largest n such that at least half the slots between 0 and n are in use.
 * Hash uses a mix of chained scatter table with Brent's variation.
 *
 * A main invariant of these tables is that, if an element is not in its main position (i.e. the original
 * position that its hash gives to it), then the colliding element is in its own main position.
 * Hence even when the load factor reaches 100%, performance remains good.
 *
 * Table keys can be arbitrary values unless they contain NaN. Keys are hashed and compared using raw equality,
 * so even if the key is a userdata with an overridden __eq, it's not used during hash lookups.
 *
 * Each table has a "boundary", defined as the index k where t[k] ~= nil and t[k+1] == nil. The boundary can be
 * computed using a binary search and can be adjusted when the table is modified; crucially, Luau enforces an
 * invariant where the boundary must be in the array part - this enforces a consistent iteration order through the
 * prefix of the table when using pairs(), and allows to implement algorithms that access elements in 1..#t range
 * more efficiently.
 */

#include "ltable.h"

#include "lobject.h"
#include "ldebug.h"
#include "lgc.h"
#include "lmem.h"
#include "lnumutils.h"
#include "ltm.h"

#include <string.h>

LUAU_FASTFLAG(LuauFrozenMetaButterfly)
LUAU_DYNAMIC_FASTFLAGVARIABLE(LuauSplitTableLookups, false)
LUAU_FASTFLAGVARIABLE(LuauTableArrayAdjustCheck)
LUAU_FASTFLAGVARIABLE(LuauTableArrayShrinkOrder)
LUAU_DYNAMIC_FASTFLAGVARIABLE(LuauTableRobustOom, false)

// Set this to 1 to change the hash function to something different (and possibly trivial). Useful
// for checking if a Lua program's behavior depends on the hash function.
#define ALT_HASH_FUNCTION (0)

// max size of both array and hash part is 2^MAXBITS
#define MAXBITS 26
#define MAXSIZE (1 << MAXBITS)

static_assert(offsetof(LuaNode, val) == 0, "Unexpected Node memory layout, pointer cast in gval2slot is incorrect");

// TKey is bitpacked for memory efficiency so we need to validate bit counts for worst case
static_assert(TKey{{NULL}, {0}, LUA_TDEADKEY, 0}.tt == LUA_TDEADKEY, "not enough bits for tt");
static_assert(TKey{{NULL}, {0}, LUA_TNIL, MAXSIZE - 1}.next == MAXSIZE - 1, "not enough bits for next");
static_assert(TKey{{NULL}, {0}, LUA_TNIL, -(MAXSIZE - 1)}.next == -(MAXSIZE - 1), "not enough bits for next");

// empty hash data points to dummynode so that we can always dereference it
const LuaNode luaH_dummynode = {
    {{NULL}, {0}, LUA_TNIL},   // value
    {{NULL}, {0}, LUA_TNIL, 0} // key
};

#define dummynode (&luaH_dummynode)

// hash is always reduced mod 2^k
#define hashpow2(t, n) (gnode(t, lmod((n), sizenode(t))))

#define hashstr(t, str) hashpow2(t, (ALT_HASH_FUNCTION ? 0x87654321 : 0) ^ (str)->hash)
#define hashboolean(t, p) hashpow2(t, (ALT_HASH_FUNCTION ? 0x87654321 : 0) ^ p)

static LuaNode* hashpointer(const LuaTable* t, const void* p)
{
    // we discard the high 32-bit portion of the pointer on 64-bit platforms as it doesn't carry much entropy anyway
    unsigned int h = unsigned(uintptr_t(p));

    // MurmurHash3 32-bit finalizer
    if (!ALT_HASH_FUNCTION)
    {
        h ^= h >> 16;
        h *= 0x85ebca6bu;
        h ^= h >> 13;
        h *= 0xc2b2ae35u;
        h ^= h >> 16;
    }

    return hashpow2(t, h);
}

static LuaNode* hashnum(const LuaTable* t, double n)
{
    static_assert(sizeof(double) == sizeof(unsigned int) * 2, "expected a 8-byte double");
    unsigned int i[2];
    memcpy(i, &n, sizeof(i));

    // mask out sign bit to make sure -0 and 0 hash to the same value
    uint32_t h1 = i[0];
    uint32_t h2 = i[1] & 0x7fffffff;

    // finalizer from MurmurHash64B
    const uint32_t m = 0x5bd1e995;

    if (!ALT_HASH_FUNCTION)
    {
        h1 ^= h2 >> 18;
        h1 *= m;
        h2 ^= h1 >> 22;
        h2 *= m;
        h1 ^= h2 >> 17;
        h1 *= m;
        h2 ^= h1 >> 19;
        h2 *= m;
    }

    // ... truncated to 32-bit output (normally hash is equal to (uint64_t(h1) << 32) | h2, but we only really need the lower 32-bit half)
    return hashpow2(t, h2);
}

static LuaNode* hashint(const LuaTable* t, int64_t n)
{
    static_assert(sizeof(n) == sizeof(unsigned int) * 2, "expected a 8-byte integer");
    unsigned int i[2];
    memcpy(i, &n, sizeof(i));

    uint32_t h1 = i[0];
    uint32_t h2 = i[1];

    // finalizer from MurmurHash64B
    const uint32_t m = 0x5bd1e995;

    if (!ALT_HASH_FUNCTION)
    {
        h1 ^= h2 >> 18;
        h1 *= m;
        h2 ^= h1 >> 22;
        h2 *= m;
        h1 ^= h2 >> 17;
        h1 *= m;
        h2 ^= h1 >> 19;
        h2 *= m;
    }

    // ... truncated to 32-bit output (normally hash is equal to (uint64_t(h1) << 32) | h2, but we only really need the lower 32-bit half)
    return hashpow2(t, h2);
}

LUAU_MAYBE_UNUSED static LuaNode* hashvec(const LuaTable* t, const float* v)
{
    unsigned int i[LUA_VECTOR_SIZE];
    memcpy(i, v, sizeof(i));

    // convert -0 to 0 to make sure they hash to the same value
    i[0] = (i[0] == 0x80000000) ? 0 : i[0];
    i[1] = (i[1] == 0x80000000) ? 0 : i[1];
    i[2] = (i[2] == 0x80000000) ? 0 : i[2];

    // scramble bits to make sure that integer coordinates have entropy in lower bits
    if (!ALT_HASH_FUNCTION)
    {
        i[0] ^= i[0] >> 17;
        i[1] ^= i[1] >> 17;
        i[2] ^= i[2] >> 17;
    }

    // Optimized Spatial Hashing for Collision Detection of Deformable Objects
    unsigned int h = (i[0] * 73856093) ^ (i[1] * 19349663) ^ (i[2] * 83492791);

#if LUA_VECTOR_SIZE == 4
    i[3] = (i[3] == 0x80000000) ? 0 : i[3];
    i[3] ^= i[3] >> 17;
    h ^= i[3] * 39916801;
#endif

    return hashpow2(t, h);
}

static LUAU_MAYBE_UNUSED LuaNode* hashvec(const LuaTable* t, const double* v)
{
    uint64_t i[LUA_VECTOR_SIZE];
    memcpy(i, v, sizeof(i));

    // convert -0 to 0 to make sure they hash to the same value
    i[0] = (i[0] == 0x8000000000000000ull) ? 0 : i[0];
    i[1] = (i[1] == 0x8000000000000000ull) ? 0 : i[1];
    i[2] = (i[2] == 0x8000000000000000ull) ? 0 : i[2];

    // scramble bits to make sure that integer coordinates have entropy in lower bits
    if (!ALT_HASH_FUNCTION)
    {
        i[0] ^= i[0] >> 32;
        i[1] ^= i[1] >> 32;
        i[2] ^= i[2] >> 32;
    }

    // Optimized Spatial Hashing for Collision Detection of Deformable Objects
    unsigned int h = uint32_t(i[0] * 73856093) ^ uint32_t(i[1] * 19349663) ^ uint32_t(i[2] * 83492791);

#if LUA_VECTOR_SIZE == 4
    i[3] = (i[3] == 0x8000000000000000ull) ? 0 : i[3];
    i[3] ^= i[3] >> 32;
    h ^= uint32_t(i[3] * 39916801);
#endif

    return hashpow2(t, h);
}

/*
** returns the `main' position of an element in a table (that is, the index
** of its hash value)
*/
static LuaNode* mainposition(const LuaTable* t, const TValue* key)
{
    switch (ttype(key))
    {
    case LUA_TNUMBER:
        return hashnum(t, nvalue(key));
    case LUA_TINTEGER:
        return hashint(t, lvalue(key));
    case LUA_TVECTOR:
        return hashvec(t, vvalue(key));
    case LUA_TSTRING:
        return hashstr(t, tsvalue(key));
    case LUA_TBOOLEAN:
        return hashboolean(t, bvalue(key));
    case LUA_TLIGHTUSERDATA:
        return hashpointer(t, pvalue(key));
    default:
        return hashpointer(t, gcvalue(key));
    }
}

static const int LUA_TGCO = -1; // Marker for template dispatch into a generic GCObject* version of a function (not for LUA_TSTRING!)

// For static mainposition dispatch on a known tag (or GCO)
template<int Tag, typename T>
static LUAU_FORCEINLINE LuaNode* mainpositiontagged(const LuaTable* t, T key) = delete;

template<>
LUAU_FORCEINLINE LuaNode* mainpositiontagged<LUA_TBOOLEAN>(const LuaTable* t, int key)
{
    return hashboolean(t, key);
}

template<>
LUAU_FORCEINLINE LuaNode* mainpositiontagged<LUA_TLIGHTUSERDATA>(const LuaTable* t, void* key)
{
    return hashpointer(t, key);
}

template<>
LUAU_FORCEINLINE LuaNode* mainpositiontagged<LUA_TNUMBER>(const LuaTable* t, double key)
{
    return hashnum(t, key);
}

template<>
LUAU_FORCEINLINE LuaNode* mainpositiontagged<LUA_TINTEGER>(const LuaTable* t, int64_t key)
{
    return hashint(t, key);
}

template<>
LUAU_FORCEINLINE LuaNode* mainpositiontagged<LUA_TVECTOR>(const LuaTable* t, const LUA_VECTOR_TYPE* key)
{
    return hashvec(t, key);
}

template<>
LUAU_FORCEINLINE LuaNode* mainpositiontagged<LUA_TSTRING>(const LuaTable* t, TString* key)
{
    return hashstr(t, key);
}

template<>
LUAU_FORCEINLINE LuaNode* mainpositiontagged<LUA_TGCO>(const LuaTable* t, GCObject* key)
{
    return hashpointer(t, key);
}

// For static luaO_rawequalKey dispatch on a known tag (or GCO). Only used specializations are implemented
template<int Tag, typename T>
static LUAU_FORCEINLINE int rawequalkeyvalue(const TKey* t1, T key) = delete;

template<>
LUAU_FORCEINLINE int rawequalkeyvalue<LUA_TBOOLEAN>(const TKey* t1, int key)
{
    return bvalue(t1) == key;
}

template<>
LUAU_FORCEINLINE int rawequalkeyvalue<LUA_TNUMBER>(const TKey* t1, double key)
{
    return luai_numeq(nvalue(t1), key);
}

template<>
LUAU_FORCEINLINE int rawequalkeyvalue<LUA_TINTEGER>(const TKey* t1, int64_t key)
{
    return luai_inteq(lvalue(t1), key);
}

template<>
LUAU_FORCEINLINE int rawequalkeyvalue<LUA_TVECTOR>(const TKey* t1, const LUA_VECTOR_TYPE* key)
{
    return luai_veceq(vvalue(t1), key);
}

template<>
LUAU_FORCEINLINE int rawequalkeyvalue<LUA_TGCO>(const TKey* t1, GCObject* key)
{
    return gcvalue(t1) == key;
}

// Lookup a node in the main position of the key or look through the node chain
template<int Tag, typename T>
static LUAU_FORCEINLINE const TValue* chainsearchtagged(const LuaTable* t, T key, int keytag)
{
    LuaNode* n = mainpositiontagged<Tag>(t, key);

    // check whether `key' is somewhere in the chain
    for (;;)
    {
        if (ttype(gkey(n)) == keytag && rawequalkeyvalue<Tag>(gkey(n), key))
            return gval(n);

        if (gnext(n) == 0)
            break;
        n += gnext(n);
    }

    return luaO_nilobject;
}

/*
** returns the index for `key' if `key' is an appropriate key to live in
** the array part of the table, -1 otherwise.
*/
static int arrayindex(double key)
{
    int i;
    luai_num2int(i, key);

    return luai_numeq(cast_num(i), key) ? i : -1;
}

/*
** returns the index of a `key' for table traversals. First goes all
** elements in the array part, then elements in the hash part. The
** beginning of a traversal is signalled by -1.
*/
static int findindex(lua_State* L, LuaTable* t, StkId key)
{
    int i;
    if (ttisnil(key))
        return -1; // first iteration
    i = ttisnumber(key) ? arrayindex(nvalue(key)) : -1;
    if (0 < i && i <= t->sizearray) // is `key' inside array part?
        return i - 1;               // yes; that's the index (corrected to C)
    else
    {
        LuaNode* n = mainposition(t, key);
        for (;;)
        { // check whether `key' is somewhere in the chain
            // key may be dead already, but it is ok to use it in `next'
            if (luaO_rawequalKey(gkey(n), key) || (ttype(gkey(n)) == LUA_TDEADKEY && iscollectable(key) && gcvalue(gkey(n)) == gcvalue(key)))
            {
                i = cast_int(n - gnode(t, 0)); // key index in hash table
                // hash elements are numbered after array ones
                return i + t->sizearray;
            }
            if (gnext(n) == 0)
                break;
            n += gnext(n);
        }
        luaG_runerror(L, "invalid key to 'next'"); // key not found
    }
}

int luaH_next(lua_State* L, LuaTable* t, StkId key)
{
    int i = findindex(L, t, key); // find original element
    for (i++; i < t->sizearray; i++)
    { // try first array part
        if (!ttisnil(&t->array[i]))
        { // a non-nil value?
            setnvalue(key, cast_num(i + 1));
            setobj2s(L, key + 1, &t->array[i]);
            return 1;
        }
    }
    for (i -= t->sizearray; i < sizenode(t); i++)
    { // then hash part
        if (!ttisnil(gval(gnode(t, i))))
        { // a non-nil value?
            getnodekey(L, key, gnode(t, i));
            setobj2s(L, key + 1, gval(gnode(t, i)));
            return 1;
        }
    }
    return 0; // no more elements
}

/*
** {=============================================================
** Rehash
** ==============================================================
*/

#define maybesetaboundary(t, boundary) \
    { \
        if (t->aboundary <= 0) \
            t->aboundary = -int(boundary); \
    }

#define getaboundary(t) (t->aboundary < 0 ? -t->aboundary : t->sizearray)

static int computesizes(int nums[], int* narray)
{
    int i;
    int twotoi; // 2^i
    int a = 0;  // number of elements smaller than 2^i
    int na = 0; // number of elements to go to array part
    int n = 0;  // optimal size for array part
    for (i = 0, twotoi = 1; twotoi / 2 < *narray; i++, twotoi *= 2)
    {
        if (nums[i] > 0)
        {
            a += nums[i];
            if (a > twotoi / 2)
            {               // more than half elements present?
                n = twotoi; // optimal size (till now)
                na = a;     // all elements smaller than n will go to array part
            }
        }
        if (a == *narray)
            break; // all elements already counted
    }
    *narray = n;
    LUAU_ASSERT(*narray / 2 <= na && na <= *narray);
    return na;
}

static int countint(double key, int* nums)
{
    int k = arrayindex(key);
    if (0 < k && k <= MAXSIZE)
    {                        // is `key' an appropriate array index?
        nums[ceillog2(k)]++; // count as such
        return 1;
    }
    else
        return 0;
}

static int numusearray(const LuaTable* t, int* nums)
{
    int lg;
    int ttlg;     // 2^lg
    int ause = 0; // summation of `nums'
    int i = 1;    // count to traverse all array keys
    for (lg = 0, ttlg = 1; lg <= MAXBITS; lg++, ttlg *= 2)
    {               // for each slice
        int lc = 0; // counter
        int lim = ttlg;
        if (lim > t->sizearray)
        {
            lim = t->sizearray; // adjust upper limit
            if (i > lim)
                break; // no more elements to count
        }
        // count elements in range (2^(lg-1), 2^lg]
        for (; i <= lim; i++)
        {
            if (!ttisnil(&t->array[i - 1]))
                lc++;
        }
        nums[lg] += lc;
        ause += lc;
    }
    return ause;
}

static int numusehash(const LuaTable* t, int* nums, int* pnasize)
{
    int totaluse = 0; // total number of elements
    int ause = 0;     // summation of `nums'
    int i = sizenode(t);
    while (i--)
    {
        LuaNode* n = &t->node[i];
        if (!ttisnil(gval(n)))
        {
            if (ttisnumber(gkey(n)))
                ause += countint(nvalue(gkey(n)), nums);
            totaluse++;
        }
    }
    *pnasize += ause;
    return totaluse;
}

static int hashslotsused(const LuaTable* t)
{
    int totaluse = 0;
    int i = sizenode(t);

    while (i--)
        totaluse += !ttisnil(gval(&t->node[i]));

    return totaluse;
}

static void setarrayvector(lua_State* L, LuaTable* t, int size)
{
    if (size > MAXSIZE)
        luaG_runerror(L, "table overflow");
    luaM_reallocarray(L, t->array, t->sizearray, size, TValue, t->memcat);
    TValue* array = t->array;
    for (int i = t->sizearray; i < size; i++)
        setnilvalue(&array[i]);
    t->sizearray = size;
}

static void setnodevector(lua_State* L, LuaTable* t, int size)
{
    int lsize;
    if (size == 0)
    {                                           // no elements to hash part?
        t->node = cast_to(LuaNode*, dummynode); // use common `dummynode'
        lsize = 0;
    }
    else
    {
        int i;
        lsize = ceillog2(size);
        if (lsize > MAXBITS)
            luaG_runerror(L, "table overflow");
        size = twoto(lsize);
        t->node = luaM_newarray(L, size, LuaNode, t->memcat);
        for (i = 0; i < size; i++)
        {
            LuaNode* n = gnode(t, i);
            gnext(n) = 0;
            setnilvalue(gkey(n));
            setnilvalue(gval(n));
        }
    }
    t->lsizenode = cast_byte(lsize);
    t->nodemask8 = cast_byte((1 << lsize) - 1);
    t->lastfree = size; // all positions are free
}

// TODO: forward declaration not needed with DFFlagLuauSplitTableLookups removal
static TValue* newkey_DEPRECATED(lua_State* L, LuaTable* t, const TValue* key);

static TValue* reinsertkey(lua_State* L, LuaTable* t, const TValue* key, bool checkarraypart);

// TODO: remove with DFFlagLuauSplitTableLookups
static TValue* arrayornewkey_DEPRECATED(lua_State* L, LuaTable* t, const TValue* key)
{
    if (ttisnumber(key))
    {
        int k;
        double n = nvalue(key);
        luai_num2int(k, n);
        if (luai_numeq(cast_num(k), n) && unsigned(k) - 1 < unsigned(t->sizearray))
            return &t->array[k - 1];
    }

    return newkey_DEPRECATED(L, t, key);
}

static void resize(lua_State* L, LuaTable* t, int nasize, int nhsize)
{
    LUAU_ASSERT(!hasmetacache(t));

    if (nasize > MAXSIZE || nhsize > MAXSIZE)
        luaG_runerror(L, "table overflow");

    if (DFFlag::LuauTableRobustOom)
    {
        // check for node size bit mask overflow and round up requested hash size
        int lsizenode = 0;

        LUAU_ASSERT(nhsize >= 0);
        if (nhsize != 0)
        {
            lsizenode = ceillog2(nhsize);
            if (lsizenode > MAXBITS)
                luaG_runerror(L, "table overflow");

            nhsize = twoto(lsizenode);
        }

        int oldsizearray = t->sizearray;
        int oldsizenode = twoto(t->lsizenode);
        TValue* oldarray = t->array;
        LuaNode* oldnode = t->node;

        TValue* newarray = t->array;

        if (nasize != oldsizearray)
        {
            newarray = luaM_newarray(L, nasize, TValue, t->memcat);

            if (nasize < oldsizearray)
            {
                // copy elements from remaining slice
                if (nasize != 0)
                    memcpy(newarray, oldarray, nasize * sizeof(TValue));
            }
            else
            {
                // copy old elements
                if (oldsizearray != 0)
                    memcpy(newarray, oldarray, oldsizearray * sizeof(TValue));

                // clear new elements
                for (int i = oldsizearray; i < nasize; i++)
                    setnilvalue(&newarray[i]);
            }
        }

        LuaNode* newnode = cast_to(LuaNode*, dummynode);

        if (nhsize != 0)
        {
            // if we allocated a new array part, we have to be careful to with hash part allocation failure
            if (oldarray != newarray)
            {
                newnode = luaM_trynewarray(L, nhsize, LuaNode, t->memcat);

                if (newnode == nullptr)
                {
                    luaM_freearray(L, newarray, nasize, TValue, t->memcat);
                    luaD_throw(L, LUA_ERRMEM);
                }
            }
            else
            {
                newnode = luaM_newarray(L, nhsize, LuaNode, t->memcat);
            }

            for (int i = 0; i < nhsize; i++)
            {
                LuaNode* n = &newnode[i];
                gnext(n) = 0;
                setnilvalue(gkey(n));
                setnilvalue(gval(n));
            }
        }

        // update table structure now that both allocations succeeded
        t->array = newarray;
        t->sizearray = nasize;

        t->node = newnode;
        t->lsizenode = cast_byte(lsizenode);
        t->nodemask8 = cast_byte((1 << lsizenode) - 1);
        t->lastfree = nhsize; // all positions are free

        // re-insert elements from vanishing array slice
        for (int i = nasize; i < oldsizearray; i++)
        {
            if (!ttisnil(&oldarray[i]))
            {
                TValue ok;
                setnvalue(&ok, cast_num(i + 1));
                if (DFFlag::LuauSplitTableLookups)
                {
                    // integer number keys from a shrinking slice cannot get into the new array part
                    setobjt2t(L, reinsertkey(L, t, &ok, false), &oldarray[i]);
                }
                else
                {
                    setobjt2t(L, newkey_DEPRECATED(L, t, &ok), &oldarray[i]);
                }
            }
        }

        // re-insert elements from hash part
        for (int i = oldsizenode - 1; i >= 0; i--)
        {
            LuaNode* old = oldnode + i;
            if (!ttisnil(gval(old)))
            {
                TValue ok;
                getnodekey(L, &ok, old);

                if (DFFlag::LuauSplitTableLookups)
                {
                    setobjt2t(L, reinsertkey(L, t, &ok, true), gval(old));
                }
                else
                {
                    setobjt2t(L, arrayornewkey_DEPRECATED(L, t, &ok), gval(old));
                }
            }
        }

        // make sure we haven't recursively rehashed during element migration
        LUAU_ASSERT(newnode == t->node);
        LUAU_ASSERT(newarray == t->array);

        // free old array
        if (nasize != oldsizearray)
            luaM_freearray(L, oldarray, oldsizearray, TValue, t->memcat);

        // free old node
        if (oldnode != dummynode)
            luaM_freearray(L, oldnode, oldsizenode, LuaNode, t->memcat);

        return;
    }

    int oldasize = t->sizearray;
    int oldhsize = t->lsizenode;
    LuaNode* nold = t->node; // save old hash ...
    if (nasize > oldasize)   // array part must grow?
        setarrayvector(L, t, nasize);
    // create new hash part with appropriate size
    setnodevector(L, t, nhsize);
    // used for the migration check at the end
    LuaNode* nnew = t->node;
    if (nasize < oldasize)
    { // array part must shrink?
        if (FFlag::LuauTableArrayShrinkOrder)
        {
            TValue* oldarray = t->array;

            // shrink array
            t->array = luaM_newarray(L, nasize, TValue, t->memcat);
            t->sizearray = nasize;

            // copy elements from remaining slice
            if (nasize != 0)
                memcpy(t->array, oldarray, nasize * sizeof(TValue));

            // re-insert elements from vanishing slice (landing into the hash part)
            for (int i = nasize; i < oldasize; i++)
            {
                if (!ttisnil(&oldarray[i]))
                {
                    TValue ok;
                    setnvalue(&ok, cast_num(i + 1));
                    if (DFFlag::LuauSplitTableLookups)
                    {
                        // integer number keys from a shrinking slice cannot get into the new array part
                        setobjt2t(L, reinsertkey(L, t, &ok, false), &oldarray[i]);
                    }
                    else
                    {
                        setobjt2t(L, newkey_DEPRECATED(L, t, &ok), &oldarray[i]);
                    }
                }
            }

            luaM_freearray(L, oldarray, oldasize, TValue, t->memcat);
        }
        else
        {
            t->sizearray = nasize;
            // re-insert elements from vanishing slice
            for (int i = nasize; i < oldasize; i++)
            {
                if (!ttisnil(&t->array[i]))
                {
                    TValue ok;
                    setnvalue(&ok, cast_num(i + 1));

                    if (DFFlag::LuauSplitTableLookups)
                    {
                        // integer number keys from a shrinking slice cannot get into the new array part
                        setobjt2t(L, reinsertkey(L, t, &ok, false), &t->array[i]);
                    }
                    else
                    {
                        setobjt2t(L, newkey_DEPRECATED(L, t, &ok), &t->array[i]);
                    }
                }
            }
            // shrink array
            luaM_reallocarray(L, t->array, oldasize, nasize, TValue, t->memcat);
        }
    }
    // used for the migration check at the end
    TValue* anew = t->array;
    // re-insert elements from hash part
    for (int i = twoto(oldhsize) - 1; i >= 0; i--)
    {
        LuaNode* old = nold + i;
        if (!ttisnil(gval(old)))
        {
            TValue ok;
            getnodekey(L, &ok, old);

            if (DFFlag::LuauSplitTableLookups)
            {
                setobjt2t(L, reinsertkey(L, t, &ok, true), gval(old));
            }
            else
            {
                setobjt2t(L, arrayornewkey_DEPRECATED(L, t, &ok), gval(old));
            }
        }
    }

    // make sure we haven't recursively rehashed during element migration
    LUAU_ASSERT(nnew == t->node);
    LUAU_ASSERT(anew == t->array);

    if (nold != dummynode)
        luaM_freearray(L, nold, twoto(oldhsize), LuaNode, t->memcat); // free old array
}

static int adjustasize_DEPRECATED(LuaTable* t, int size, const TValue* ek)
{
    bool tbound = t->node != dummynode || size < t->sizearray;
    int ekindex = ek && ttisnumber(ek) ? arrayindex(nvalue(ek)) : -1;
    // move the array size up until the boundary is guaranteed to be inside the array part
    while (size + 1 == ekindex || (tbound && !ttisnil(luaH_getnum(t, size + 1))))
        size++;
    return size;
}

// When we already know that new key is an integer number
static int adjustasize(LuaTable* t, int size, int extraintkey)
{
    bool tbound = t->node != dummynode || size < t->sizearray;
    // move the array size up until the boundary is guaranteed to be inside the array part
    while (size + 1 == extraintkey || (tbound && !ttisnil(luaH_getnum(t, size + 1))))
        size++;
    return size;
}

void luaH_resizearray(lua_State* L, LuaTable* t, int nasize)
{
    if (FFlag::LuauTableArrayAdjustCheck && nasize > MAXSIZE)
        luaG_runerror(L, "table overflow");

    int nsize = (t->node == dummynode) ? 0 : sizenode(t);
    int asize = DFFlag::LuauSplitTableLookups ? adjustasize(t, nasize, -1) : adjustasize_DEPRECATED(t, nasize, NULL);
    resize(L, t, asize, nsize);
}

void luaH_resizehash(lua_State* L, LuaTable* t, int nhsize)
{
    resize(L, t, t->sizearray, nhsize);
}

static void rehash_DEPRECATED(lua_State* L, LuaTable* t, const TValue* ek)
{
    int nums[MAXBITS + 1]; // nums[i] = number of keys between 2^(i-1) and 2^i
    for (int i = 0; i <= MAXBITS; i++)
        nums[i] = 0;                          // reset counts
    int nasize = numusearray(t, nums);        // count keys in array part
    int totaluse = nasize;                    // all those keys are integer keys
    totaluse += numusehash(t, nums, &nasize); // count keys in hash part

    // count extra key
    if (ttisnumber(ek))
        nasize += countint(nvalue(ek), nums);
    totaluse++;

    // compute new size for array part
    int na = computesizes(nums, &nasize);
    int nh = totaluse - na;

    // enforce the boundary invariant; for performance, only do hash lookups if we must
    int nadjusted = adjustasize_DEPRECATED(t, nasize, ek);

    // count how many extra elements belong to array part instead of hash part
    int aextra = nadjusted - nasize;

    if (aextra != 0)
    {
        // we no longer need to store those extra array elements in hash part
        nh -= aextra;

        // because hash nodes are twice as large as array nodes, the memory we saved for hash parts can be used by array part
        // this follows the general sparse array part optimization where array is allocated when 50% occupation is reached
        nasize = nadjusted + aextra;

        // since the size was changed, it's again important to enforce the boundary invariant at the new size
        nasize = adjustasize_DEPRECATED(t, nasize, ek);
    }

    // resize the table to new computed sizes
    resize(L, t, nasize, nh);
}

template<bool IsIntKey>
static void rehash(lua_State* L, LuaTable* t, int newintkey)
{
    // insertions of keys that are not integer numbers do not have to rehash the array part
    // this can mean that such key insert will not cause the sparse array/node rebalance, but this is actually less surprising
    if (!IsIntKey)
    {
        int nh = hashslotsused(t) + 1;
        resize(L, t, t->sizearray, nh);
        return;
    }

    // nums[i] = number of keys between 2^(i-1) and 2^i
    int nums[MAXBITS + 1];
    for (int i = 0; i <= MAXBITS; i++)
        nums[i] = 0;

    int nasize = 0;
    int totaluse = 0;

    if (t->sizearray != 0)
    {
        nasize = numusearray(t, nums); // count keys in array part
        totaluse = nasize;             // all those keys are integer keys
    }

    if (t->node != dummynode)
        totaluse += numusehash(t, nums, &nasize); // count keys in hash part

    // count extra key
    if (IsIntKey && 0 < newintkey && newintkey <= MAXSIZE)
    {
        nums[ceillog2(newintkey)]++;
        nasize++;
    }

    totaluse++;

    // compute new size for array part
    int na = computesizes(nums, &nasize);
    int nh = totaluse - na;

    // enforce the boundary invariant; for performance, only do hash lookups if we must
    int nadjusted = adjustasize(t, nasize, newintkey);

    // count how many extra elements belong to array part instead of hash part
    int aextra = nadjusted - nasize;

    if (aextra != 0)
    {
        // we no longer need to store those extra array elements in hash part
        nh -= aextra;

        // because hash nodes are twice as large as array nodes, the memory we saved for hash parts can be used by array part
        // this follows the general sparse array part optimization where array is allocated when 50% occupation is reached
        nasize = nadjusted + aextra;

        // since the size was changed, it's again important to enforce the boundary invariant at the new size
        nasize = adjustasize(t, nasize, newintkey);
    }

    // resize the table to new computed sizes
    resize(L, t, nasize, nh);
}

/*
** }=============================================================
*/

LuaTable* luaH_new(lua_State* L, int narray, int nhash)
{
    LuaTable* t = luaM_newgco(L, LuaTable, sizeof(LuaTable), L->activememcat, LUA_TTABLE);
    luaC_init(L, t, LUA_TTABLE);
    t->metatable = NULL;
    t->tmcache = cast_byte(~0);
    t->array = NULL;
    t->sizearray = 0;
    t->lastfree = 0;
    t->lsizenode = 0;
    t->readonly = 0;
    t->safeenv = 0;
    t->nodemask8 = 0;
    t->node = cast_to(LuaNode*, dummynode);
    if (narray > 0)
        setarrayvector(L, t, narray);
    if (nhash > 0)
        setnodevector(L, t, nhash);
    return t;
}

void luaH_free(lua_State* L, LuaTable* t, lua_Page* page)
{
    if (t->node != dummynode)
        luaM_freearray(L, t->node, sizenode(t), LuaNode, t->memcat);

    if (FFlag::LuauFrozenMetaButterfly)
    {
        if (t->array)
        {
            if (hasmetacache(t))
                luaM_freearray(L, t->array - TM_N, t->sizearray + TM_N, TValue, t->memcat);
            else
                luaM_freearray(L, t->array, t->sizearray, TValue, t->memcat);
        }
    }
    else
    {
        if (t->array)
            luaM_freearray(L, t->array, t->sizearray, TValue, t->memcat);
    }

    luaM_freegco(L, t, sizeof(LuaTable), t->memcat, page);
}

static LuaNode* getfreepos(LuaTable* t)
{
    while (t->lastfree > 0)
    {
        t->lastfree--;

        LuaNode* n = gnode(t, t->lastfree);
        if (ttisnil(gkey(n)))
            return n;
    }
    return NULL; // could not find a free place
}

static LuaNode* newcollidingkey(lua_State* L, LuaTable* t, LuaNode* mp, LuaNode* n)
{
    LUAU_ASSERT(n != dummynode);

    TValue mk;
    getnodekey(L, &mk, mp);
    LuaNode* othern = mainposition(t, &mk);

    // is colliding node out of its main position?
    if (othern != mp)
    {
        // yes, move colliding node into free position
        while (othern + gnext(othern) != mp)
            othern += gnext(othern); // find previous

        gnext(othern) = cast_int(n - othern); // redo the chain with `n' in place of `mp'
        *n = *mp;                             // copy colliding node into free pos. (mp->next also goes)

        if (gnext(mp) != 0)
        {
            gnext(n) += cast_int(mp - n); // correct 'next'
            gnext(mp) = 0;                // now 'mp' is free
        }

        setnilvalue(gval(mp));
        return mp;
    }
    else
    {
        // colliding node is in its own main position
        // new node will go into free position
        if (gnext(mp) != 0)
            gnext(n) = cast_int((mp + gnext(mp)) - n); // chain new position
        else
            LUAU_ASSERT(gnext(n) == 0);
        gnext(mp) = cast_int(n - mp);
        return n;
    }
}

static TValue* newkey_DEPRECATED(lua_State* L, LuaTable* t, const TValue* key)
{
    // enforce boundary invariant
    if (ttisnumber(key) && nvalue(key) == t->sizearray + 1)
    {
        rehash_DEPRECATED(L, t, key); // grow table

        // after rehash, numeric keys might be located in the new array part, but won't be found in the node part
        return arrayornewkey_DEPRECATED(L, t, key);
    }

    LuaNode* mp = mainposition(t, key);
    if (!ttisnil(gval(mp)) || mp == dummynode)
    {
        LuaNode* n = getfreepos(t); // get a free place
        if (n == NULL)
        {                      // cannot find a free place?
            rehash_DEPRECATED(L, t, key); // grow table

            // after rehash, numeric keys might be located in the new array part, but won't be found in the node part
            return arrayornewkey_DEPRECATED(L, t, key);
        }
        LUAU_ASSERT(n != dummynode);
        TValue mk;
        getnodekey(L, &mk, mp);
        LuaNode* othern = mainposition(t, &mk);
        if (othern != mp)
        { // is colliding node out of its main position?
            // yes; move colliding node into free position
            while (othern + gnext(othern) != mp)
                othern += gnext(othern);          // find previous
            gnext(othern) = cast_int(n - othern); // redo the chain with `n' in place of `mp'
            *n = *mp;                             // copy colliding node into free pos. (mp->next also goes)
            if (gnext(mp) != 0)
            {
                gnext(n) += cast_int(mp - n); // correct 'next'
                gnext(mp) = 0;                // now 'mp' is free
            }
            setnilvalue(gval(mp));
        }
        else
        { // colliding node is in its own main position
            // new node will go into free position
            if (gnext(mp) != 0)
                gnext(n) = cast_int((mp + gnext(mp)) - n); // chain new position
            else
                LUAU_ASSERT(gnext(n) == 0);
            gnext(mp) = cast_int(n - mp);
            mp = n;
        }
    }

    setnodekey(L, mp, key);
    luaC_barriert(L, t, key);
    LUAU_ASSERT(ttisnil(gval(mp)));
    return gval(mp);
}

/*
** inserts a new key into a hash table; first, check whether key's main
** position is free. If not, check whether colliding node is in its main
** position or not: if it is not, move colliding node to an empty place and
** put new key in its main position; otherwise (colliding node is in its main
** position), new key goes to an empty position.
*/
template<int Tag, bool IsIntKey, typename Key>
static LUAU_NOINLINE TValue* newkeytagged(lua_State* L, LuaTable* t, const TValue* key, Key hashkey, int newintkey)
{
    // enforce boundary invariant
    if (IsIntKey && newintkey == t->sizearray + 1)
    {
        rehash<IsIntKey>(L, t, newintkey); // grow table

        if (unsigned(newintkey) - 1 < unsigned(t->sizearray))
            return &t->array[newintkey - 1];

        // after rehash, numeric keys might be located in the new array part, but won't be found in the node part
        return newkeytagged<Tag, IsIntKey>(L, t, key, hashkey, newintkey);
    }

    LuaNode* mp = mainpositiontagged<Tag>(t, hashkey);

    if (!ttisnil(gval(mp)) || mp == dummynode)
    {
        LuaNode* n = getfreepos(t); // get a free place

        // cannot find a free place?
        if (n == nullptr)
        {
            rehash<IsIntKey>(L, t, newintkey); // grow table

            if (IsIntKey && unsigned(newintkey) - 1 < unsigned(t->sizearray))
                return &t->array[newintkey - 1];

            // after rehash, numeric keys might be located in the new array part, but won't be found in the node part
            return newkeytagged<Tag, IsIntKey>(L, t, key, hashkey, newintkey);
        }

        mp = newcollidingkey(L, t, mp, n);
    }

    setnodekey(L, mp, key);

    if (Tag >= LUA_TSTRING || Tag == LUA_TGCO)
        luaC_barriert(L, t, key);

    LUAU_ASSERT(ttisnil(gval(mp)));
    return gval(mp);
}

// for the table rehash path, we cannot trigger nested reshashing
static TValue* reinsertkey(lua_State* L, LuaTable* t, const TValue* key, bool checkarraypart)
{
    if (checkarraypart && ttisnumber(key))
    {
        int k;
        double n = nvalue(key);
        luai_num2int(k, n);
        if (luai_numeq(cast_num(k), n) && unsigned(k) - 1 < unsigned(t->sizearray))
            return &t->array[k - 1];
    }

    LuaNode* mp = mainposition(t, key);

    if (!ttisnil(gval(mp)) || mp == dummynode)
        mp = newcollidingkey(L, t, mp, getfreepos(t));

    setnodekey(L, mp, key);
    luaC_barriert(L, t, key);
    LUAU_ASSERT(ttisnil(gval(mp)));
    return gval(mp);
}

/*
** search function for integers
*/
const TValue* luaH_getnum(LuaTable* t, int key)
{
    // (1 <= key && key <= t->sizearray)
    if (unsigned(key) - 1 < unsigned(t->sizearray))
        return &t->array[key - 1];
    else if (t->node != dummynode)
    {
        double nk = cast_num(key);
        LuaNode* n = hashnum(t, nk);
        for (;;)
        { // check whether `key' is somewhere in the chain
            if (ttisnumber(gkey(n)) && luai_numeq(nvalue(gkey(n)), nk))
                return gval(n); // that's it
            if (gnext(n) == 0)
                break;
            n += gnext(n);
        }
        return luaO_nilobject;
    }
    else
        return luaO_nilobject;
}

/*
** search function for strings
*/
const TValue* luaH_getstr(LuaTable* t, TString* key)
{
    LuaNode* n = hashstr(t, key);
    for (;;)
    { // check whether `key' is somewhere in the chain
        if (ttisstring(gkey(n)) && tsvalue(gkey(n)) == key)
            return gval(n); // that's it
        if (gnext(n) == 0)
            break;
        n += gnext(n);
    }
    return luaO_nilobject;
}

/*
** search function for lightuserdata
*/
const TValue* luaH_getp(LuaTable* t, void* key, int tag)
{
    LuaNode* n = hashpointer(t, key);
    for (;;)
    { // check whether `key' is somewhere in the chain
        const TKey* nk = gkey(n);
        if (ttislightuserdata(nk) && pvalue(nk) == key && lightuserdatatag(nk) == tag)
            return gval(n); // that's it
        if (gnext(n) == 0)
            break;
        n += gnext(n);
    }
    return luaO_nilobject;
}

/*
** main search function
*/
const TValue* luaH_get(LuaTable* t, const TValue* key)
{
    if (DFFlag::LuauSplitTableLookups)
    {
        switch (ttype(key))
        {
        case LUA_TNIL:
            return luaO_nilobject;
        case LUA_TBOOLEAN:
            return chainsearchtagged<LUA_TBOOLEAN>(t, bvalue(key), LUA_TBOOLEAN);
        case LUA_TLIGHTUSERDATA:
            return luaH_getp(t, pvalue(key), lightuserdatatag(key)); // special search function with tag comparison
        case LUA_TNUMBER:
        {
            int k;
            double n = nvalue(key);
            luai_num2int(k, n);
            if (luai_numeq(cast_num(k), nvalue(key))) // index is int?
                return luaH_getnum(t, k);             // use specialized version

            return chainsearchtagged<LUA_TNUMBER>(t, nvalue(key), LUA_TNUMBER);
        }
        case LUA_TINTEGER:
            return chainsearchtagged<LUA_TINTEGER>(t, lvalue(key), LUA_TINTEGER);
        case LUA_TVECTOR:
            return chainsearchtagged<LUA_TVECTOR>(t, (const LUA_VECTOR_TYPE*)vvalue(key), LUA_TVECTOR);
        case LUA_TSTRING:
            return luaH_getstr(t, tsvalue(key));
        default:
            return chainsearchtagged<LUA_TGCO>(t, gcvalue(key), ttype(key));
        }
    }
    else
    {
        switch (ttype(key))
        {
        case LUA_TNIL:
            return luaO_nilobject;
        case LUA_TSTRING:
            return luaH_getstr(t, tsvalue(key));
        case LUA_TNUMBER:
        {
            int k;
            double n = nvalue(key);
            luai_num2int(k, n);
            if (luai_numeq(cast_num(k), nvalue(key))) // index is int?
                return luaH_getnum(t, k);             // use specialized version
            LUAU_FALLTHROUGH;                         // else go through
        }
        default:
        {
            LuaNode* n = mainposition(t, key);
            for (;;)
            { // check whether `key' is somewhere in the chain
                if (luaO_rawequalKey(gkey(n), key))
                    return gval(n); // that's it
                if (gnext(n) == 0)
                    break;
                n += gnext(n);
            }
            return luaO_nilobject;
        }
        }
    }
}

TValue* luaH_set(lua_State* L, LuaTable* t, const TValue* key)
{
    const TValue* p = luaH_get(t, key);
    invalidateTMcache(t);
    if (p != luaO_nilobject)
        return cast_to(TValue*, p);
    else
        return luaH_newkey(L, t, key);
}

TValue* luaH_newkey(lua_State* L, LuaTable* t, const TValue* key)
{
    if (DFFlag::LuauSplitTableLookups)
    {
        switch (ttype(key))
        {
        case LUA_TNIL:
            luaG_runerror(L, "table index is nil");
            break;
        case LUA_TBOOLEAN:
            return newkeytagged<LUA_TBOOLEAN, false>(L, t, key, bvalue(key), -1);
        case LUA_TLIGHTUSERDATA:
            return newkeytagged<LUA_TLIGHTUSERDATA, false>(L, t, key, pvalue(key), -1);
        case LUA_TNUMBER:
        {
            double n = nvalue(key);

            if (luai_numisnan(n))
                luaG_runerror(L, "table index is NaN");

            int k;
            luai_num2int(k, n);
            if (luai_numeq(cast_num(k), n))
                return newkeytagged<LUA_TNUMBER, true>(L, t, key, n, k);

            return newkeytagged<LUA_TNUMBER, false>(L, t, key, n, -1);
        }
        case LUA_TINTEGER:
            return newkeytagged<LUA_TINTEGER, false>(L, t, key, lvalue(key), -1);
        case LUA_TVECTOR:
            if (luai_vecisnan(vvalue(key)))
                luaG_runerror(L, "table index contains NaN");

            return newkeytagged<LUA_TVECTOR, false>(L, t, key, (const LUA_VECTOR_TYPE*)vvalue(key), -1);
        case LUA_TSTRING:
            return newkeytagged<LUA_TSTRING, false>(L, t, key, tsvalue(key), -1);
        default:
            return newkeytagged<LUA_TGCO, false>(L, t, key, gcvalue(key), -1);
        }
    }
    else
    {
        if (ttisnil(key))
            luaG_runerror(L, "table index is nil");
        else if (ttisnumber(key) && luai_numisnan(nvalue(key)))
            luaG_runerror(L, "table index is NaN");
        else if (ttisvector(key) && luai_vecisnan(vvalue(key)))
            luaG_runerror(L, "table index contains NaN");
        return newkey_DEPRECATED(L, t, key);
    }
}

TValue* luaH_setnum(lua_State* L, LuaTable* t, int key)
{
    // (1 <= key && key <= t->sizearray)
    if (unsigned(key) - 1 < unsigned(t->sizearray))
        return &t->array[key - 1];
    // hash fallback
    const TValue* p = luaH_getnum(t, key);
    if (p != luaO_nilobject)
        return cast_to(TValue*, p);
    else
    {
        if (DFFlag::LuauSplitTableLookups)
        {
            double n = cast_num(key);

            TValue k;
            setnvalue(&k, n);
            return newkeytagged<LUA_TNUMBER, true>(L, t, &k, n, key);
        }
        else
        {
            TValue k;
            setnvalue(&k, cast_num(key));
            return newkey_DEPRECATED(L, t, &k);
        }
    }
}

TValue* luaH_setstr(lua_State* L, LuaTable* t, TString* key)
{
    const TValue* p = luaH_getstr(t, key);
    invalidateTMcache(t);
    if (p != luaO_nilobject)
        return cast_to(TValue*, p);
    else
    {
        TValue k;
        setsvalue(L, &k, key);

        if (DFFlag::LuauSplitTableLookups)
            return newkeytagged<LUA_TSTRING, false>(L, t, &k, key, -1);
        else
            return newkey_DEPRECATED(L, t, &k);
    }
}

TValue* luaH_setp(lua_State* L, LuaTable* t, void* key, int tag)
{
    const TValue* p = luaH_getp(t, key, tag);
    if (p != luaO_nilobject)
        return cast_to(TValue*, p);
    else
    {
        TValue k;
        setpvalue(&k, key, tag);

        if (DFFlag::LuauSplitTableLookups)
            return newkeytagged<LUA_TLIGHTUSERDATA, false>(L, t, &k, key, -1);
        else
            return newkey_DEPRECATED(L, t, &k);
    }
}

static int updateaboundary(LuaTable* t, int boundary)
{
    if (boundary < t->sizearray && ttisnil(&t->array[boundary - 1]))
    {
        if (boundary >= 2 && !ttisnil(&t->array[boundary - 2]))
        {
            maybesetaboundary(t, boundary - 1);
            return boundary - 1;
        }
    }
    else if (boundary + 1 < t->sizearray && !ttisnil(&t->array[boundary]) && ttisnil(&t->array[boundary + 1]))
    {
        maybesetaboundary(t, boundary + 1);
        return boundary + 1;
    }

    return 0;
}

/*
** Try to find a boundary in table `t'. A `boundary' is an integer index
** such that t[i] is non-nil and t[i+1] is nil (and 0 if t[1] is nil).
*/
int luaH_getn(LuaTable* t)
{
    int boundary = getaboundary(t);

    if (boundary > 0)
    {
        if (!ttisnil(&t->array[t->sizearray - 1]) && t->node == dummynode)
            return t->sizearray; // fast-path: the end of the array in `t' already refers to a boundary
        if (boundary < t->sizearray && !ttisnil(&t->array[boundary - 1]) && ttisnil(&t->array[boundary]))
            return boundary; // fast-path: boundary already refers to a boundary in `t'

        int foundboundary = updateaboundary(t, boundary);
        if (foundboundary > 0)
            return foundboundary;
    }

    int j = t->sizearray;

    if (j > 0 && ttisnil(&t->array[j - 1]))
    {
        // "branchless" binary search from Array Layouts for Comparison-Based Searching, Paul Khuong, Pat Morin, 2017.
        // note that clang is cmov-shy on cmovs around memory operands, so it will compile this to a branchy loop.
        TValue* base = t->array;
        int rest = j;
        while (int half = rest >> 1)
        {
            base = ttisnil(&base[half]) ? base : base + half;
            rest -= half;
        }
        int boundary = !ttisnil(base) + int(base - t->array);
        maybesetaboundary(t, boundary);
        return boundary;
    }
    else
    {
        // validate boundary invariant
        LUAU_ASSERT(t->node == dummynode || ttisnil(luaH_getnum(t, j + 1)));
        return j;
    }
}

LuaTable* luaH_clone(lua_State* L, LuaTable* tt)
{
    LuaTable* t = luaM_newgco(L, LuaTable, sizeof(LuaTable), L->activememcat, LUA_TTABLE);
    luaC_init(L, t, LUA_TTABLE);
    t->metatable = tt->metatable;
    t->tmcache = tt->tmcache;
    t->array = NULL;
    t->sizearray = 0;
    t->lsizenode = 0;
    t->nodemask8 = 0;
    t->readonly = 0;
    t->safeenv = 0;
    t->node = cast_to(LuaNode*, dummynode);
    t->lastfree = 0;

    if (tt->sizearray)
    {
        t->array = luaM_newarray(L, tt->sizearray, TValue, t->memcat);
        maybesetaboundary(t, getaboundary(tt));
        t->sizearray = tt->sizearray;

        memcpy(t->array, tt->array, t->sizearray * sizeof(TValue));
    }

    if (tt->node != dummynode)
    {
        int size = 1 << tt->lsizenode;
        t->node = luaM_newarray(L, size, LuaNode, t->memcat);
        t->lsizenode = tt->lsizenode;
        t->nodemask8 = tt->nodemask8;
        memcpy(t->node, tt->node, size * sizeof(LuaNode));
        t->lastfree = tt->lastfree;
    }

    return t;
}

void luaH_clear(LuaTable* tt)
{
    // clear array part
    for (int i = 0; i < tt->sizearray; ++i)
    {
        setnilvalue(&tt->array[i]);
    }

    maybesetaboundary(tt, 0);

    // clear hash part
    if (tt->node != dummynode)
    {
        int size = sizenode(tt);
        tt->lastfree = size;
        for (int i = 0; i < size; ++i)
        {
            LuaNode* n = gnode(tt, i);
            setnilvalue(gkey(n));
            setnilvalue(gval(n));
            gnext(n) = 0;
        }
    }

    // back to empty -> no tag methods present
    tt->tmcache = cast_byte(~0);
}

void luaH_setreadonly(lua_State* L, LuaTable* t, bool readonly)
{
    if (luaH_getreadonly(t) == (readonly ? 1 : 0))
        return;

    if (readonly)
    {
        t->readonly = 1;

        // check if this table is likely to be used as a metatable
        if (t->node == dummynode)
            return;

        // collect all metamethods
        TValue metamethods[TM_N];
        bool found = false;
        uint8_t tmcache = 0;

        for (int i = 0; i < TM_N; i++)
        {
            const TValue* val = luaH_getstr(t, L->global->tmname[i]);
            setobj(L, &metamethods[TM_N - (i + 1)], val); // metamethods will be stored in reverse
            bool hasValue = !ttisnil(val);
            found |= hasValue;

            if (i <= TM_EQ && !hasValue)
                tmcache |= cast_byte(1u << i);
        }

        // update the metamethod presence cache
        t->tmcache = tmcache;

        // if there are metamethods, place the metamethod cache before the array elements
        if (found)
        {
            if (DFFlag::LuauTableRobustOom)
            {
                // if allocation fails, the optimization is skipped instead of erroring as an error might be unexpected from a table freeze
                if (TValue* newarray = luaM_trynewarray(L, t->sizearray + TM_N, TValue, t->memcat))
                {
                    TValue* oldarray = t->array;

                    if (t->sizearray != 0)
                        memcpy(newarray + TM_N, oldarray, t->sizearray * sizeof(TValue));

                    memcpy(newarray, metamethods, sizeof(metamethods));
                    t->array = newarray + TM_N;
                    t->readonly |= 2;

                    luaM_freearray(L, oldarray, t->sizearray, TValue, t->memcat);
                }
            }
            else
            {
                TValue* array = t->array;
                luaM_reallocarray(L, array, t->sizearray, t->sizearray + TM_N, TValue, t->memcat);
                memmove(array + TM_N, array, t->sizearray * sizeof(TValue));
                memcpy(array, metamethods, sizeof(metamethods));
                t->array = array + TM_N;
                t->readonly |= 2;
            }
        }
    }
    else
    {
        // if we have a metamethod cache, transform the array part back to the original layout (or delete when sizearray is 0)
        if (hasmetacache(t))
        {
            TValue* oldarray = t->array;
            TValue* newarray = nullptr;

            if (t->sizearray != 0)
            {
                newarray = luaM_newarray(L, t->sizearray, TValue, t->memcat);
                memcpy(newarray, oldarray, t->sizearray * sizeof(TValue));
            }

            t->array = newarray;
            luaM_freearray(L, oldarray - TM_N, t->sizearray + TM_N, TValue, t->memcat);
        }

        t->readonly = 0;
    }
}

int luaH_getreadonly(LuaTable* t)
{
    return t->readonly != 0 ? 1 : 0;
}
