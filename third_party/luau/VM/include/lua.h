// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "luaconf.h"



// option for multiple returns in `lua_pcall' and `lua_call'
#define LUA_MULTRET (-1)

/*
** pseudo-indices
*/
#define LUA_REGISTRYINDEX (-LUAI_MAXCSTACK - 2000)
#define LUA_ENVIRONINDEX (-LUAI_MAXCSTACK - 2001)
#define LUA_GLOBALSINDEX (-LUAI_MAXCSTACK - 2002)
#define lua_upvalueindex(i) (LUA_GLOBALSINDEX - (i))
#define lua_ispseudo(i) ((i) <= LUA_REGISTRYINDEX)

// thread status; 0 is OK
enum lua_Status
{
    LUA_OK = 0,
    LUA_YIELD,
    LUA_ERRRUN,
    LUA_ERRSYNTAX, // legacy error code, preserved for compatibility
    LUA_ERRMEM,
    LUA_ERRERR,
    LUA_BREAK, // yielded for a debug breakpoint
};

enum lua_CoStatus
{
    LUA_CORUN = 0, // running
    LUA_COSUS,     // suspended
    LUA_CONOR,     // 'normal' (it resumed another coroutine)
    LUA_COFIN,     // finished
    LUA_COERR,     // finished with error
};

typedef struct lua_State lua_State;

typedef int (*lua_CFunction)(lua_State* L);
typedef int (*lua_Continuation)(lua_State* L, int status);

/*
** prototype for memory-allocation functions
*/

typedef void* (*lua_Alloc)(void* ud, void* ptr, size_t osize, size_t nsize);

// `type` is an opaque VM-defined value which identifies the caged heap allocation kind
// separating allocations of different types is recommended
typedef void* (*lua_CageAlloc)(void* ud, void* ptr, size_t osize, size_t nsize, int type);

// non-return type
#define l_noret void LUA_NORETURN

/*
** basic types
*/
#define LUA_TNONE (-1)

/*
 * WARNING: if you change the order of this enumeration,
 * grep "ORDER TYPE"
 */
// clang-format off
enum lua_Type
{
    LUA_TNIL = 0,     // must be 0 due to lua_isnoneornil
    LUA_TBOOLEAN = 1, // must be 1 due to l_isfalse

    LUA_TLIGHTUSERDATA,
    LUA_TNUMBER,
    LUA_TINTEGER,

    // When vector components are 'float', vector fits into the TValue
#if LUA_VECTOR_DOUBLE == 0
    LUA_TVECTOR,
#endif

    LUA_TSTRING, // all types above this must be value types, all types below this must be GC types - see iscollectable

    LUA_TTABLE,
    LUA_TFUNCTION,
    LUA_TUSERDATA,
    LUA_TTHREAD,
    LUA_TBUFFER,
    LUA_TCLASS,
    LUA_TOBJECT,

    // When vector components are 'double', it is a GCObject with heap-allocated data
#if LUA_VECTOR_DOUBLE == 1
    LUA_TVECTOR,
#endif

    // values below this line are used in GCObject tags but may never show up in TValue type tags

    // LUA_TDEADKEY is used in TKey to identify Luau table entries that have the value set to nil,
    // so that we can remove the strong reference to the key.
    LUA_TDEADKEY,

    // These values should never show up in TValue tag types.
    LUA_TPROTO,
    LUA_TUPVAL,

    // the count of all Luau types (including those that are never TValue type tags)
    LUA_T_ALL,

    // the count of TValue type tags
    LUA_T_COUNT = LUA_TDEADKEY,
};
// clang-format on

// type of numbers in Luau
typedef double lua_Number;

// type for integer functions
typedef int lua_Integer;

// unsigned integer type
typedef unsigned lua_Unsigned;

/*
** state manipulation
*/
LUA_API lua_State* lua_newstate(lua_Alloc allocator, void* ud);
LUA_API void lua_close(lua_State* L);
LUA_API lua_State* lua_newthread(lua_State* L);
LUA_API lua_State* lua_mainthread(lua_State* L);
LUA_API void lua_resetthread(lua_State* L);
LUA_API int lua_isthreadreset(lua_State* L);

/*
** basic stack manipulation
*/
LUA_API int lua_absindex(lua_State* L, int idx);
LUA_API int lua_gettop(lua_State* L);
LUA_API void lua_settop(lua_State* L, int idx);
LUA_API void lua_pushvalue(lua_State* L, int idx);
LUA_API void lua_remove(lua_State* L, int idx);
LUA_API void lua_insert(lua_State* L, int idx);
LUA_API void lua_replace(lua_State* L, int idx);
LUA_API int lua_checkstack(lua_State* L, int sz);
LUA_API void lua_rawcheckstack(lua_State* L, int sz); // allows for unlimited stack frames

LUA_API void lua_xmove(lua_State* from, lua_State* to, int n);
LUA_API void lua_xpush(lua_State* from, lua_State* to, int idx);

/*
** access functions (stack -> C)
*/

LUA_API int lua_isnumber(lua_State* L, int idx);
LUA_API int lua_isstring(lua_State* L, int idx);
LUA_API int lua_isinteger64(lua_State* L, int idx);
LUA_API int lua_iscfunction(lua_State* L, int idx);
LUA_API int lua_isLfunction(lua_State* L, int idx);
LUA_API int lua_isuserdata(lua_State* L, int idx);
LUA_API int lua_type(lua_State* L, int idx);
LUA_API const char* lua_typename(lua_State* L, int tp);

LUA_API int lua_equal(lua_State* L, int idx1, int idx2);
LUA_API int lua_rawequal(lua_State* L, int idx1, int idx2);
LUA_API int lua_lessthan(lua_State* L, int idx1, int idx2);

LUA_API double lua_tonumberx(lua_State* L, int idx, int* isnum);
LUA_API int lua_tointegerx(lua_State* L, int idx, int* isnum);
LUA_API unsigned lua_tounsignedx(lua_State* L, int idx, int* isnum);
LUA_API const LUA_VECTOR_TYPE* lua_tovector(lua_State* L, int idx);
LUA_API int lua_toboolean(lua_State* L, int idx);
LUA_API int64_t lua_tointeger64(lua_State* L, int idx, int* isinteger);
LUA_API const char* lua_tolstring(lua_State* L, int idx, size_t* len);
LUA_API const char* lua_tostringatom(lua_State* L, int idx, int* atom);
LUA_API const char* lua_tolstringatom(lua_State* L, int idx, size_t* len, int* atom);
LUA_API const char* lua_namecallatom(lua_State* L, int* atom);
LUA_API int lua_objlen(lua_State* L, int idx);
LUA_API lua_CFunction lua_tocfunction(lua_State* L, int idx);
LUA_API void* lua_tolightuserdata(lua_State* L, int idx);
LUA_API void* lua_tolightuserdatatagged(lua_State* L, int idx, int tag);
LUA_API void* lua_touserdata(lua_State* L, int idx);
LUA_API void* lua_touserdatatagged(lua_State* L, int idx, int tag);
LUA_API int lua_userdatatag(lua_State* L, int idx);
LUA_API int lua_lightuserdatatag(lua_State* L, int idx);
LUA_API lua_State* lua_tothread(lua_State* L, int idx);
LUA_API void* lua_tobuffer(lua_State* L, int idx, size_t* len);
LUA_API const void* lua_topointer(lua_State* L, int idx);

/*
** push functions (C -> stack)
*/
LUA_API void lua_pushnil(lua_State* L);
LUA_API void lua_pushnumber(lua_State* L, double n);
LUA_API void lua_pushinteger(lua_State* L, int n);
LUA_API void lua_pushinteger64(lua_State* L, int64_t n);
LUA_API void lua_pushunsigned(lua_State* L, unsigned n);
#if LUA_VECTOR_SIZE == 4
LUA_API void lua_pushvector(lua_State* L, LUA_VECTOR_TYPE x, LUA_VECTOR_TYPE y, LUA_VECTOR_TYPE z, LUA_VECTOR_TYPE w);
#else
LUA_API void lua_pushvector(lua_State* L, LUA_VECTOR_TYPE x, LUA_VECTOR_TYPE y, LUA_VECTOR_TYPE z);
#endif
LUA_API void lua_pushlstring(lua_State* L, const char* s, size_t l);
LUA_API void lua_pushstring(lua_State* L, const char* s);
LUA_API const char* lua_pushvfstring(lua_State* L, const char* fmt, va_list argp);
LUA_API LUA_PRINTF_ATTR(2, 3) const char* lua_pushfstringL(lua_State* L, const char* fmt, ...);
LUA_API void lua_pushcclosurek(lua_State* L, lua_CFunction fn, const char* debugname, int nup, lua_Continuation cont);
LUA_API void lua_pushboolean(lua_State* L, int b);
LUA_API int lua_pushthread(lua_State* L);

typedef void (*lua_Destructor)(lua_State* L, void* userdata);

LUA_API void lua_pushlightuserdatatagged(lua_State* L, void* p, int tag);
LUA_API void* lua_newuserdatatagged(lua_State* L, size_t sz, int tag);
LUA_API void* lua_newuserdatataggedwithmetatable(lua_State* L, size_t sz, int tag); // metatable fetched with lua_getuserdatametatable
LUA_API void* lua_newuserdatadtor(lua_State* L, size_t sz, lua_Destructor dtor);

LUA_API void* lua_newbuffer(lua_State* L, size_t sz);

/*
** get functions (Lua -> stack)
*/
LUA_API int lua_gettable(lua_State* L, int idx);
LUA_API int lua_getfield(lua_State* L, int idx, const char* k);
LUA_API int lua_rawgetfield(lua_State* L, int idx, const char* k);
LUA_API int lua_rawget(lua_State* L, int idx);
LUA_API int lua_rawgeti(lua_State* L, int idx, int n);
LUA_API int lua_rawgetptagged(lua_State* L, int idx, void* p, int tag);
LUA_API void lua_createtable(lua_State* L, int narr, int nrec);

LUA_API void lua_setreadonly(lua_State* L, int idx, int enabled);
LUA_API int lua_getreadonly(lua_State* L, int idx);
LUA_API void lua_setsafeenv(lua_State* L, int idx, int enabled);

LUA_API int lua_getmetatable(lua_State* L, int objindex);
LUA_API void lua_getfenv(lua_State* L, int idx);

/*
** set functions (stack -> Lua)
*/
LUA_API void lua_settable(lua_State* L, int idx);
LUA_API void lua_setfield(lua_State* L, int idx, const char* k);
LUA_API void lua_rawsetfield(lua_State* L, int idx, const char* k);
LUA_API void lua_rawset(lua_State* L, int idx);
LUA_API void lua_rawseti(lua_State* L, int idx, int n);
LUA_API void lua_rawsetptagged(lua_State* L, int idx, void* p, int tag);
LUA_API int lua_setmetatable(lua_State* L, int objindex);
LUA_API int lua_setfenv(lua_State* L, int idx);

/*
** `load' and `call' functions (load and run Luau bytecode)
*/
LUA_API int luau_load(lua_State* L, const char* chunkname, const char* data, size_t size, int env);
LUA_API void lua_call(lua_State* L, int nargs, int nresults);
LUA_API int lua_pcall(lua_State* L, int nargs, int nresults, int errfunc);
LUA_API int lua_cpcall(lua_State* L, lua_CFunction func, void* ud);

// wrapper for making calls from yieldable C functions
LUA_API int lua_callyieldable(lua_State* L, int nargs, int nresults);
LUA_API int lua_pcallyieldable(lua_State* L, int nargs, int nresults, int errfunc);

/*
** coroutine functions
*/
LUA_API int lua_yield(lua_State* L, int nresults);
LUA_API int lua_break(lua_State* L);
LUA_API int lua_resume(lua_State* L, lua_State* from, int narg);
LUA_API int lua_resumeerror(lua_State* L, lua_State* from);
LUA_API int lua_status(lua_State* L);
LUA_API int lua_isyieldable(lua_State* L);
LUA_API void* lua_getthreaddata(lua_State* L);
LUA_API void lua_setthreaddata(lua_State* L, void* data);
LUA_API int lua_costatus(lua_State* L, lua_State* co);
LUA_API int lua_hasfinalizers(lua_State* L);
LUA_API void lua_pushfinalizerfunction(lua_State* L, lua_State* co, int toclose);
LUA_API void lua_addfinalizer(lua_State* L, lua_State* co, int idx);

/*
** garbage-collection function and options
*/

enum lua_GCOp
{
    // stop and resume incremental garbage collection
    LUA_GCSTOP,
    LUA_GCRESTART,

    // run a full GC cycle; not recommended for latency sensitive applications
    LUA_GCCOLLECT,

    // return the heap size in KB and the remainder in bytes
    LUA_GCCOUNT,
    LUA_GCCOUNTB,

    // return 1 if GC is active (not stopped); note that GC may not be actively collecting even if it's running
    LUA_GCISRUNNING,

    /*
    ** perform an explicit GC step, with the step size specified in KB
    **
    ** garbage collection is handled by 'assists' that perform some amount of GC work matching pace of allocation
    ** explicit GC steps allow to perform some amount of work at custom points to offset the need for GC assists
    ** note that GC might also be paused for some duration (until bytes allocated meet the threshold)
    ** if an explicit step is performed during this pause, it will trigger the start of the next collection cycle
    */
    LUA_GCSTEP,

    /*
    ** tune GC parameters G (goal), S (step multiplier) and step size (usually best left ignored)
    **
    ** garbage collection is incremental and tries to maintain the heap size to balance memory and performance overhead
    ** this overhead is determined by G (goal) which is the ratio between total heap size and the amount of live data in it
    ** G is specified in percentages; by default G=200% which means that the heap is allowed to grow to ~2x the size of live data.
    **
    ** collector tries to collect S% of allocated bytes by interrupting the application after step size bytes were allocated.
    ** when S is too small, collector may not be able to catch up and the effective goal that can be reached will be larger.
    ** S is specified in percentages; by default S=200% which means that collector will run at ~2x the pace of allocations.
    **
    ** it is recommended to set S in the interval [100 / (G - 100), 100 + 100 / (G - 100))] with a minimum value of 150%; for example:
    ** - for G=200%, S should be in the interval [150%, 200%]
    ** - for G=150%, S should be in the interval [200%, 300%]
    ** - for G=125%, S should be in the interval [400%, 500%]
    */
    LUA_GCSETGOAL,
    LUA_GCSETSTEPMUL,
    LUA_GCSETSTEPSIZE,

    // return 1 if GC is in a paused state; making a GC step in a paused state will unpause the GC
    LUA_GCISPAUSED,
};

LUA_API int lua_gc(lua_State* L, int what, int data);

typedef const char* (*lua_CategoryName)(lua_State* L, uint8_t memcat);

// write a Luau memory dump to a FILE in JSON format
// categoryName callback, when provided, will be called to record a name associated with the category
LUA_API void lua_memorydump(lua_State* L, void* file, lua_CategoryName categoryName);

/*
** memory statistics
** all allocated bytes are attributed to the memory category of the running thread (0..LUA_MEMORY_CATEGORIES-1)
*/

LUA_API void lua_setmemcat(lua_State* L, int category);
LUA_API size_t lua_totalbytes(lua_State* L, int category);

// measure the allocation rate in bytes/sec
// returns -1 if allocation rate cannot be measured
LUA_API int64_t lua_allocationrate(lua_State* L);

/*
** miscellaneous functions
*/

LUA_API l_noret lua_error(lua_State* L);

LUA_API int lua_next(lua_State* L, int idx);
LUA_API int lua_rawiter(lua_State* L, int idx, int iter);

LUA_API void lua_concat(lua_State* L, int n);

LUA_API void lua_setpointerencodekey(lua_State* L, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
LUA_API uintptr_t lua_encodepointer(lua_State* L, uintptr_t p);

LUA_API double lua_clock();

LUA_API void lua_setuserdatatag(lua_State* L, int idx, int tag);

LUA_API void lua_setuserdatadtor(lua_State* L, int tag, lua_Destructor dtor);
LUA_API lua_Destructor lua_getuserdatadtor(lua_State* L, int tag);

// Embedder GC integration APIs
//
// Luau -> embedder (lua_setuserdatamark): when the GC marks a tagged userdata,
// the registered callback fires so the embedder can mark the corresponding
// native object as reachable.
//
// Embedder -> Luau (lua_setembeddergc): the GC calls this each cycle.
// Two cases:
// 1. Cycle reset (markref == NULL): clear accumulated information about marked
//      native objects.
// 2. Mark phase (markref != NULL): embedder must call markref for each new ref
//      that is reachable from a marked native object.
//
// Together, these are used to implement a fixed-point reachability algorithm
// across both heaps.
//
// Note: because these functions are called during garbage collection, they
// must not perform any reentrant operations on the lua_State. Only truly
// read-only APIs like lua_getthreaddata are safe to call from here.
typedef void (*lua_UserdataMark)(lua_State* L, void* ud);
LUA_API void lua_setuserdatamark(lua_State* L, int tag, lua_UserdataMark markfn);
typedef void (*lua_EmbedderMark)(lua_State* L, int ref);
typedef void (*lua_EmbedderGc)(lua_State* L, lua_EmbedderMark markref);
LUA_API void lua_setembeddergc(lua_State* L, lua_EmbedderGc fn);

// APIs for interacting with embedder-managed weak references.
//
// Unlike lua_ref, these do not prevent collection on their own; the embedder
// must call markref(L, ref) inside lua_EmbedderGc to keep them alive. If the
// lua_getweakref API returns nil when the embedder expects the object to be
// alive, it indicates a bug in the embedder's logic.
//
// Bugs in the embedder's marking logic can manifest in two ways:
// - the native object owning a ref was not marked (Luau -> embedder), or
// - if the native object was marked, the ref was not marked (embedder -> Luau).
//
// Otherwise, a non-marking-related bug could simply be that the embedder is
// trying to access a ref that was correctly collected. Specifically, if a
// native object that logically owns a ref is destroyed, then the embedder
// should not be trying to access that ref anymore.
//
// In any case, asserting on the return value of lua_getweakref is a good way
// for embedders to catch bugs in their own marking logic.
LUA_API int lua_weakref(lua_State* L, int idx);
LUA_API int lua_weakunref(lua_State* L, int ref);
LUA_API int lua_getweakref(lua_State* L, int ref); // returns the type of the value pushed onto the stack

// alternative access for userdata metatables
// used by lua_newuserdatataggedwithmetatable to create tagged userdata with the associated metatable assigned
LUA_API void lua_setuserdatametatable(lua_State* L, int tag);
LUA_API void lua_getuserdatametatable(lua_State* L, int tag);

// Returns the name of a userdata tag - `__type` from the metatable or "userdata" if it is not set
LUA_API const char* lua_getuserdataname(lua_State* L, int tag);

// NOTE: experimental API and is subject to breaking changes
// registration of callbacks for direct userdata __index, __newindex and __namecall access with string keys assigned with an atom
// cachedslot is initially 0 and can be set to a custom value to help with data lookup inside the userdata
// IMPORTANT: cachedslot values are shared between all userdata, callbacks function of one userdata tag has to correctly handle values set by another
typedef void (*lua_UserdataDirectAccess)(lua_State* L, void* data, int atom, uint16_t* cachedslot, int utag);
typedef int (*lua_UserdataDirectNamecall)(lua_State* L, void* data, int atom, uint16_t* cachedslot, int utag);

LUA_API int lua_registeruserdatadirectaccess(
    lua_State* L,
    int tag,
    lua_UserdataDirectAccess get,
    lua_UserdataDirectAccess set,
    lua_UserdataDirectNamecall namecall
);

/*
** Direct field API
**
** lua_registeruserdatadirectfieldget registers a per-field, per-userdata-type
** handler that is invoked directly without allocating a Luau call frame.
**
** tag:   userdata tag (0..LUA_UTAG_LIMIT-1)
** field: field name string (will be interned and pinned)
** fn:    handler — receives raw userdata data pointer and result TValue slot
*/
typedef void (*lua_UserdataDirectFieldGet)(void* ud, void* result);
LUA_API void lua_registeruserdatadirectfieldget(lua_State* L, int tag, const char* field, lua_UserdataDirectFieldGet fn);

// Helpers for writing result values from a direct field handler.
LUA_API void lua_userdatadirectfield_setnumber(void* result, double n);
#if LUA_VECTOR_SIZE == 4
LUA_API void lua_userdatadirectfield_setvector(void* result, LUA_VECTOR_TYPE x, LUA_VECTOR_TYPE y, LUA_VECTOR_TYPE z, LUA_VECTOR_TYPE w);
#else
LUA_API void lua_userdatadirectfield_setvector(void* result, LUA_VECTOR_TYPE x, LUA_VECTOR_TYPE y, LUA_VECTOR_TYPE z);
#endif
LUA_API void lua_userdatadirectfield_setboolean(void* result, int b);
LUA_API void lua_userdatadirectfield_setinteger64(void* result, int64_t n);
LUA_API void lua_userdatadirectfield_setnil(void* result);

LUA_API void lua_setlightuserdataname(lua_State* L, int tag, const char* name);
LUA_API const char* lua_getlightuserdataname(lua_State* L, int tag);

LUA_API void lua_clonefunction(lua_State* L, int idx);
LUA_API int lua_usesexport(lua_State* L, int idx);

LUA_API void lua_cleartable(lua_State* L, int idx);
LUA_API void lua_clonetable(lua_State* L, int idx);

LUA_API lua_Alloc lua_getallocf(lua_State* L, void** ud);

/*
** reference system, can be used to pin objects
*/
#define LUA_NOREF -1
#define LUA_REFNIL 0

LUA_API int lua_ref(lua_State* L, int idx);
LUA_API int lua_unref(lua_State* L, int ref);

#define lua_getref(L, ref) lua_rawgeti(L, LUA_REGISTRYINDEX, (ref))

/*
** ===============================================================
** some useful macros
** ===============================================================
*/
#define lua_tonumber(L, i) lua_tonumberx(L, i, NULL)
#define lua_tointeger(L, i) lua_tointegerx(L, i, NULL)
#define lua_tounsigned(L, i) lua_tounsignedx(L, i, NULL)

#define lua_pop(L, n) lua_settop(L, -(n)-1)

#define lua_newtable(L) lua_createtable(L, 0, 0)
#define lua_newuserdata(L, s) lua_newuserdatatagged(L, s, 0)

#define lua_strlen(L, i) lua_objlen(L, (i))

#define lua_isfunction(L, n) (lua_type(L, (n)) == LUA_TFUNCTION)
#define lua_istable(L, n) (lua_type(L, (n)) == LUA_TTABLE)
#define lua_islightuserdata(L, n) (lua_type(L, (n)) == LUA_TLIGHTUSERDATA)
#define lua_isnil(L, n) (lua_type(L, (n)) == LUA_TNIL)
#define lua_isboolean(L, n) (lua_type(L, (n)) == LUA_TBOOLEAN)
#define lua_isinteger64(L, n) (lua_type(L, (n)) == LUA_TINTEGER)
#define lua_isvector(L, n) (lua_type(L, (n)) == LUA_TVECTOR)
#define lua_isthread(L, n) (lua_type(L, (n)) == LUA_TTHREAD)
#define lua_isbuffer(L, n) (lua_type(L, (n)) == LUA_TBUFFER)
#define lua_isnone(L, n) (lua_type(L, (n)) == LUA_TNONE)
#define lua_isnoneornil(L, n) (lua_type(L, (n)) <= LUA_TNIL)
#define lua_isclass(L, n) (lua_type(L, (n)) == LUA_TCLASS)
#define lua_isobject(L, n) (lua_type(L, (n)) == LUA_TOBJECT)

#define lua_pushliteral(L, s) lua_pushlstring(L, "" s, (sizeof(s) / sizeof(char)) - 1)
#define lua_pushcfunction(L, fn, debugname) lua_pushcclosurek(L, fn, debugname, 0, NULL)
#define lua_pushcclosure(L, fn, debugname, nup) lua_pushcclosurek(L, fn, debugname, nup, NULL)
#define lua_pushlightuserdata(L, p) lua_pushlightuserdatatagged(L, p, 0)

#define lua_rawgetp(L, idx, p) lua_rawgetptagged(L, idx, p, 0)
#define lua_rawsetp(L, idx, p) lua_rawsetptagged(L, idx, p, 0)

#define lua_setglobal(L, s) lua_setfield(L, LUA_GLOBALSINDEX, (s))
#define lua_getglobal(L, s) lua_getfield(L, LUA_GLOBALSINDEX, (s))

#define lua_tostring(L, i) lua_tolstring(L, (i), NULL)

#define lua_pushfstring(L, fmt, ...) lua_pushfstringL(L, fmt, ##__VA_ARGS__)

/*
** {======================================================================
** Debug API
** =======================================================================
*/

typedef struct lua_Debug lua_Debug; // activation record

// Functions to be called by the debugger in specific events
typedef void (*lua_Hook)(lua_State* L, lua_Debug* ar);

// invoke a debug hook on a thread in break/yield state
// userdata is passed to hook through lua_Debug::userdata field
LUA_API void lua_callhook(lua_State* L, lua_Hook hook, void* userdata);

LUA_API int lua_stackdepth(lua_State* L);
LUA_API int lua_getinfo(lua_State* L, int level, const char* what, lua_Debug* ar);
LUA_API int lua_getargument(lua_State* L, int level, int n);
LUA_API const char* lua_getlocal(lua_State* L, int level, int n);
LUA_API const char* lua_setlocal(lua_State* L, int level, int n);
LUA_API const char* lua_getupvalue(lua_State* L, int funcindex, int n);
LUA_API const char* lua_setupvalue(lua_State* L, int funcindex, int n);

LUA_API int lua_hascustomexecution(lua_State* L, int level); // function has custom execution data set
LUA_API int lua_incustomexecution(lua_State* L, int level);  // function is running using custom execution

LUA_API void lua_singlestep(lua_State* L, int enabled);
LUA_API int lua_breakpoint(lua_State* L, int funcindex, int line, int enabled);

// returns 1 if execution is currently at a breakpoint; should only be called from debug callbacks
LUA_API int lua_atbreakpoint(lua_State* L);

// Warning: this function is not thread-safe since it stores the result in a shared global array! Only use for debugging.
LUA_API const char* lua_debugtrace(lua_State* L);

struct lua_Debug
{
    const char* name;      // (n)
    const char* what;      // (s) `Lua', `C', `main', `tail'
    const char* source;    // (s)
    const char* short_src; // (s)
    int linedefined;       // (s)
    int currentline;       // (l)
    int protoid;           // (p) globally unique (within VM) proto id; 0 for C functions
    int bytecodeid;        // (p) proto index within its bytecode module; -1 for C functions
    unsigned char nupvals; // (u) number of upvalues
    unsigned char nparams; // (a) number of parameters
    char isvararg;         // (a)
    void* userdata; // only valid in lua_callhook

    char ssbuf[LUA_IDSIZE];
};

typedef void (*lua_Coverage)(void* context, const char* function, int linedefined, int depth, const int* hits, size_t size);

LUA_API void lua_getcoverage(lua_State* L, int funcindex, void* context, lua_Coverage callback);

typedef void (*lua_CounterFunction)(void* context, const char* function, int linedefined);
typedef void (*lua_CounterValue)(void* context, int kind, int line, uint64_t hits);

// Unlike 'lua_getcoverage', counters are customizable in ways which prevent merging them together
// 'lua_getcounters' will visit the specified function and all nested functions
// 'functionvisit' is called first to establish a function, then multiple calls of 'countervisit' are made for each counter in that function
LUA_API void lua_getcounters(lua_State* L, int funcindex, void* context, lua_CounterFunction functionvisit, lua_CounterValue countervisit);

// }======================================================================

/* Callbacks that can be used to reconfigure behavior of the VM dynamically.
 * These are shared between all coroutines.
 *
 * Note: interrupt is safe to set from an arbitrary thread but all other callbacks
 * can only be changed when the VM is not running any code */
struct lua_Callbacks
{
    void* userdata; // arbitrary userdata pointer that is never overwritten by Luau

    void (*interrupt)(lua_State* L, int gc);  // gets called at safepoints (loop back edges, call/ret, gc) if set
    void (*panic)(lua_State* L, int errcode); // gets called when an unprotected error is raised (if longjmp is used)

    void (*userthread)(lua_State* LP, lua_State* L); // gets called when L is created (LP == parent) or destroyed (LP == NULL)
    int16_t (*useratom)(lua_State* L, const char* s, size_t l); // gets called when a string is created to assign an atom id

    // NOTE: experimental API, requires a Debug flag to be called and is subject to breaking changes
    void (*userfinalizer)(lua_State* L, lua_State* co); // gets called before a finalizer is attached to 'co' by current thread

    void (*debugbreak)(lua_State* L, lua_Debug* ar);     // gets called when BREAK instruction is encountered
    void (*debugstep)(lua_State* L, lua_Debug* ar);      // gets called after each instruction in single step mode
    void (*debuginterrupt)(lua_State* L, lua_Debug* ar); // gets called when thread execution is interrupted by break in another thread
    void (*debugprotectederror)(lua_State* L);           // gets called when protected call results in an error

    // gets called after a heap object (or array) is allocated
    void (*onallocate)(lua_State* L, void* block, size_t osize, size_t nsize, uint8_t memcat, int tt, int tag);

    void (*preresume)(lua_State* L);  // gets called before lua_resume runs a (co)routine
    void (*postresume)(lua_State* L); // gets called after lua_resume returns (yield, return, or error)

    // gets called before a heap object (or array) is freed
    void (*onfree)(lua_State* L, void* block);
};
typedef struct lua_Callbacks lua_Callbacks;

LUA_API lua_Callbacks* lua_callbacks(lua_State* L);

// Must be called after lua_newstate and before the state creates any additional objects
// The VM makes no assumptions about the layout or structure of the caged heap
// The VM does assume that the embedder will free any memory allocated if the main lua_State the cage is associated with is closed
LUA_API void lua_setmemorycage(lua_State* L, lua_CageAlloc alloc, void* ud);

/******************************************************************************
 * Copyright (c) 2019-2023 Roblox Corporation
 * Copyright (C) 1994-2008 Lua.org, PUC-Rio.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 ******************************************************************************/
