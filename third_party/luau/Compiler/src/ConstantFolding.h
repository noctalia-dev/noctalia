// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/Compiler.h"

#include "ValueTracking.h"

#include <vector>

namespace Luau
{
namespace Compile
{

struct Constant
{
    enum Type
    {
        Type_Unknown,
        Type_Nil,
        Type_Boolean,
        Type_Number,
        Type_Integer,
        Type_Vectorf,
        Type_Vectord,
        Type_String,
        Type_Table,
    };

    Type type = Type_Unknown;
    unsigned int stringLength = 0;

    union
    {
        bool valueBoolean;
        double valueNumber;
        int64_t valueInteger64;
        float valueVectorf[4];
        double valueVectord[4];
        size_t valueTable;                 // index pointing to constant table entry with table's constant properties
        const char* valueString = nullptr; // length stored in stringLength
    };

    bool isTruthful() const
    {
        LUAU_ASSERT(type != Type_Unknown);
        return type != Type_Nil && !(type == Type_Boolean && valueBoolean == false);
    }

    AstArray<const char> getString() const
    {
        LUAU_ASSERT(type == Type_String);
        return {valueString, stringLength};
    }
};

enum TableConstantKind
{
    ConstantTable,
    NotConstant
};

void buildTableConstantMap(DenseHashMap<AstLocal*, TableConstantKind>& result, const DenseHashMap<AstLocal*, Variable>& variables, AstNode* root);

struct ExprConstantChange
{
    AstExpr* key = nullptr;
    Constant oldValue;
    bool wasAbsent = false;
};

struct LocalConstantChange
{
    AstLocal* key = nullptr;
    Constant oldValue;
    bool wasAbsent = false;
};

using ExprConstantChangeLog = std::vector<ExprConstantChange>;
using LocalConstantChangeLog = std::vector<LocalConstantChange>;

void undoChanges(DenseHashMap<AstExpr*, Constant>& constants, const ExprConstantChangeLog& changes);
void undoChanges(DenseHashMap<AstLocal*, Constant>& locals, const LocalConstantChangeLog& changes);

void foldConstants(
    DenseHashMap<AstExpr*, Constant>& constants,
    DenseHashMap<AstLocal*, Variable>& variables,
    DenseHashMap<AstLocal*, Constant>& locals,
    const DenseHashMap<AstExprCall*, int>* builtins,
    bool foldLibraryK,
    bool vectorDoublePrecision,
    LibraryMemberConstantCallback libraryMemberConstantCb,
    AstNode* root,
    AstNameTable& stringTable,
    const DenseHashMap<AstLocal*, TableConstantKind>& tableConstants,
    ExprConstantChangeLog* exprChangeLog = nullptr,
    LocalConstantChangeLog* localChangeLog = nullptr
);

} // namespace Compile
} // namespace Luau
