// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "ConstantFolding.h"

namespace Luau
{

class AstNameTable;

namespace Compile
{

Constant foldBuiltin(AstNameTable& stringTable, int bfid, const Constant* args, size_t count, bool vectorDoublePrecision);
Constant foldBuiltinMath(AstName index);

} // namespace Compile
} // namespace Luau
