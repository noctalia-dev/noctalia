// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/ParseOptions.h"
#include "Luau/Location.h"
#include "Luau/StringUtils.h"
#include "Luau/Common.h"

#include <exception>

namespace Luau
{
class AstNameTable;
struct ParseResult;
class BytecodeBuilder;
class BytecodeEncoder;

using CompileConstant = void*;

// return a type identifier for a global library member
// values are defined by 'enum LuauBytecodeType' in Bytecode.h
using LibraryMemberTypeCallback = int (*)(const char* library, const char* member);

// setup a value of a constant for a global library member
// use setCompileConstant*** set of functions for values
using LibraryMemberConstantCallback = void (*)(const char* library, const char* member, CompileConstant* constant);

// Note: this structure is duplicated in luacode.h, don't forget to change these in sync!
struct CompileOptions
{
    // 0 - no optimization
    // 1 - baseline optimization level that doesn't prevent debuggability
    // 2 - includes optimizations that harm debuggability such as inlining
    int optimizationLevel = 1;

    // 0 - no debugging support
    // 1 - line info & function names only; sufficient for backtraces
    // 2 - full debug info with local & upvalue names; necessary for debugger
    int debugLevel = 1;

    // type information is used to guide native code generation decisions
    // information includes testable typeArguments for function arguments, locals, upvalues and some temporaries
    // 0 - generate for native modules
    // 1 - generate for all modules
    int typeInfoLevel = 0;

    // 0 - no code coverage support
    // 1 - statement coverage
    // 2 - statement and expression coverage (verbose)
    int coverageLevel = 0;

    // alternative global builtin to construct vectors, in addition to default builtin 'vector.create'
    const char* vectorLib = nullptr;
    const char* vectorCtor = nullptr;

    // alternative vector type name for type tables, in addition to default type 'vector'
    const char* vectorType = nullptr;

    // 0 - 32-bit float vector components
    // 1 - 64-bit double vector components
    int vectorPrecision = 0;

    // null-terminated array of globals that are mutable; disables the import optimization for fields accessed through these
    const char* const* mutableGlobals = nullptr;

    // null-terminated array of userdata typeArguments that will be included in the type information
    const char* const* userdataTypes = nullptr;

    // null-terminated array of globals which act as libraries and have members with known type and/or constant value
    // when an import of one of these libraries is accessed, callbacks below will be called to receive that information
    const char* const* librariesWithKnownMembers = nullptr;
    LibraryMemberTypeCallback libraryMemberTypeCb = nullptr;
    LibraryMemberConstantCallback libraryMemberConstantCb = nullptr;

    // null-terminated array of library functions that should not be compiled into a built-in fastcall ("name" "lib.name")
    const char* const* disabledBuiltins = nullptr;
};

class CompileError : public std::exception
{
public:
    CompileError(const Location& location, std::string message);

    ~CompileError() throw() override;

    const char* what() const throw() override;

    const Location& getLocation() const;

    static LUAU_NORETURN void raise(const Location& location, const char* format, ...) LUAU_PRINTF_ATTR(2, 3);

private:
    Location location;
    std::string message;
};

// compiles bytecode into bytecode builder using either a pre-parsed AST or parsing it from source; throws on errors
void compileOrThrow(BytecodeBuilder& bytecode, const ParseResult& parseResult, AstNameTable& names, const CompileOptions& options = {});
void compileOrThrow(BytecodeBuilder& bytecode, const std::string& source, const CompileOptions& options = {}, const ParseOptions& parseOptions = {});

// compiles bytecode into a bytecode blob, that either contains the valid bytecode or an encoded error that luau_load can decode
std::string compile(
    const std::string& source,
    const CompileOptions& options = {},
    const ParseOptions& parseOptions = {},
    BytecodeEncoder* encoder = nullptr
);

void setCompileConstantNil(CompileConstant* constant);
void setCompileConstantBoolean(CompileConstant* constant, bool b);
void setCompileConstantNumber(CompileConstant* constant, double n);
void setCompileConstantInteger64(CompileConstant* constant, int64_t l);
void setCompileConstantVector(CompileConstant* constant, float x, float y, float z, float w);
void setCompileConstantVectord(CompileConstant* constant, double x, double y, double z, double w);
void setCompileConstantString(CompileConstant* constant, const char* s, size_t l);

} // namespace Luau
