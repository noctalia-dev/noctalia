// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "luacode.h"

#include "Luau/Compiler.h"

#include <stdlib.h>
#include <string.h>

char* luau_compile(const char* source, size_t size, lua_CompileOptions* options, size_t* outsize)
{
    LUAU_ASSERT(outsize);

    Luau::CompileOptions opts;

    if (options)
    {
        static_assert(sizeof(lua_CompileOptions) == sizeof(Luau::CompileOptions), "C and C++ interface must match");
        memcpy(static_cast<void*>(&opts), options, sizeof(opts));
    }

    std::string result = compile(std::string(source, size), opts);

    char* copy = static_cast<char*>(malloc(result.size()));
    if (!copy)
        return nullptr;

    memcpy(copy, result.data(), result.size());
    *outsize = result.size();
    return copy;
}

void luau_set_compile_constant_nil(lua_CompileConstant* constant)
{
    Luau::setCompileConstantNil(constant);
}

void luau_set_compile_constant_boolean(lua_CompileConstant* constant, int b)
{
    Luau::setCompileConstantBoolean(constant, b != 0);
}

void luau_set_compile_constant_number(lua_CompileConstant* constant, double n)
{
    Luau::setCompileConstantNumber(constant, n);
}

void luau_set_compile_constant_integer64(lua_CompileConstant* constant, int64_t l)
{
    Luau::setCompileConstantInteger64(constant, l);
}

void luau_set_compile_constant_vector(lua_CompileConstant* constant, float x, float y, float z, float w)
{
    Luau::setCompileConstantVector(constant, x, y, z, w);
}

void luau_set_compile_constant_vectord(lua_CompileConstant* constant, double x, double y, double z, double w)
{
    Luau::setCompileConstantVectord(constant, x, y, z, w);
}

void luau_set_compile_constant_string(lua_CompileConstant* constant, const char* s, size_t l)
{
    Luau::setCompileConstantString(constant, s, l);
}
