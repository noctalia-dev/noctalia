// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include <stddef.h>
#include <stdint.h>

// can be used to reconfigure visibility/exports for public APIs
#ifndef LUACODE_API
#define LUACODE_API extern
#endif

typedef struct lua_CompileOptions lua_CompileOptions;
typedef void* lua_CompileConstant;

// return a type identifier for a global library member
// values are defined by 'enum LuauBytecodeType' in Bytecode.h
typedef int (*lua_LibraryMemberTypeCallback)(const char* library, const char* member);

// setup a value of a constant for a global library member
// use luau_set_compile_constant_*** set of functions for values
typedef void (*lua_LibraryMemberConstantCallback)(const char* library, const char* member, lua_CompileConstant* constant);

struct lua_CompileOptions
{
    // 0 - no optimization
    // 1 - baseline optimization level that doesn't prevent debuggability
    // 2 - includes optimizations that harm debuggability such as inlining
    int optimizationLevel; // default=1

    // 0 - no debugging support
    // 1 - line info & function names only; sufficient for backtraces
    // 2 - full debug info with local & upvalue names; necessary for debugger
    int debugLevel; // default=1

    // type information is used to guide native code generation decisions
    // information includes testable types for function arguments, locals, upvalues and some temporaries
    // 0 - generate for native modules
    // 1 - generate for all modules
    int typeInfoLevel; // default=0

    // 0 - no code coverage support
    // 1 - statement coverage
    // 2 - statement and expression coverage (verbose)
    int coverageLevel; // default=0

    // alternative global builtin to construct vectors, in addition to default builtin 'vector.create'
    const char* vectorLib;
    const char* vectorCtor;

    // alternative vector type name for type tables, in addition to default type 'vector'
    const char* vectorType;

    // 0 - 32-bit float vector components
    // 1 - 64-bit double vector components
    int vectorPrecision; // default=0

    // null-terminated array of globals that are mutable; disables the import optimization for fields accessed through these
    const char* const* mutableGlobals;

    // null-terminated array of userdata types that will be included in the type information
    const char* const* userdataTypes;

    // null-terminated array of globals which act as libraries and have members with known type and/or constant value
    // when an import of one of these libraries is accessed, callbacks below will be called to receive that information
    const char* const* librariesWithKnownMembers;
    lua_LibraryMemberTypeCallback libraryMemberTypeCb;
    lua_LibraryMemberConstantCallback libraryMemberConstantCb;

    // null-terminated array of library functions that should not be compiled into a built-in fastcall ("name" "lib.name")
    const char* const* disabledBuiltins;
};

// compile source to bytecode; when source compilation fails, the resulting bytecode contains the encoded error. use free() to destroy
LUACODE_API char* luau_compile(const char* source, size_t size, lua_CompileOptions* options, size_t* outsize);

// when libraryMemberConstantCb is called, these methods can be used to set a value of the opaque lua_CompileConstant struct
// vector component 'w' is not visible to VM runtime configured with LUA_VECTOR_SIZE == 3, but can affect constant folding during compilation
// string storage must outlive the invocation of 'luau_compile' which used the callback
LUACODE_API void luau_set_compile_constant_nil(lua_CompileConstant* constant);
LUACODE_API void luau_set_compile_constant_boolean(lua_CompileConstant* constant, int b);
LUACODE_API void luau_set_compile_constant_number(lua_CompileConstant* constant, double n);
LUACODE_API void luau_set_compile_constant_integer64(lua_CompileConstant* constant, int64_t l);
LUACODE_API void luau_set_compile_constant_vector(lua_CompileConstant* constant, float x, float y, float z, float w);
LUACODE_API void luau_set_compile_constant_vectord(lua_CompileConstant* constant, double x, double y, double z, double w);
LUACODE_API void luau_set_compile_constant_string(lua_CompileConstant* constant, const char* s, size_t l);
