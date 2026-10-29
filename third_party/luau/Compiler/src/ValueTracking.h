// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/Ast.h"
#include "Luau/DenseHash.h"

#include <vector>

namespace Luau
{
class AstNameTable;
}

namespace Luau
{
namespace Compile
{

enum class Global
{
    Default = 0,
    Mutable, // builtin that has contents unknown at compile time, blocks GETIMPORT for chains
    Written, // written in the code which means we can't reason about the value
};

struct Variable
{
    AstExpr* init = nullptr; // initial value of the variable; filled by trackValues
    AstStatBlock* owner = nullptr;    // block statement in which the variable was defined (not necessarily directly)
    AstExprLocal* lastUsed = nullptr; // last lexical expression where the variable was read
    bool written = false;    // is the variable ever assigned to? filled by trackValues
    bool constant = false;   // is the variable's value a compile-time constant? filled by constantFold
    bool nonLexicalUse = false; // variable may be used by expressions which do not lexically mention it (like function calls)
};

void assignMutable(DenseHashMap<AstName, Global>& globals, const AstNameTable& names, const char* const* mutableGlobals);
void trackValues(
    DenseHashMap<AstName, Global>& globals,
    DenseHashMap<AstLocal*, Variable>& variables,
    DenseHashSet<AstLocal*>& exportedFunctions,
    std::vector<AstLocal*>& exportedVariables,
    AstNode* root
);
void trackValues_DEPRECATED(DenseHashMap<AstName, Global>& globals, DenseHashMap<AstLocal*, Variable>& variables, AstNode* root);

inline Global getGlobalState(const DenseHashMap<AstName, Global>& globals, AstName name)
{
    const Global* it = globals.find(name);

    return it ? *it : Global::Default;
}

} // namespace Compile
} // namespace Luau
