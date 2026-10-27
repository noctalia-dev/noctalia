// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/Ast.h"
#include "Luau/DenseHash.h"

#include <vector>

namespace Luau
{

enum class Mode
{
    NoCheck,    // Do not perform any inference
    Nonstrict,  // Unannotated symbols are any
    Strict,     // Unannotated symbols are inferred
    Definition, // Type definition module, has special parsing rules
};

struct FragmentParseResumeSettings
{
    DenseHashMap<AstName, AstLocal*> localMap;
    std::vector<AstLocal*> localStack;
    Position resumePosition;
};

struct ParseOptions
{
    bool allowDeclarationSyntax = false;
    bool captureComments = false;
    std::optional<FragmentParseResumeSettings> parseFragment = std::nullopt;
    bool storeCstData = false;
    bool noErrorLimit = false;
};

} // namespace Luau
