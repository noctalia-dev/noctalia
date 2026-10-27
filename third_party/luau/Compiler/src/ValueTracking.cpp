// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "ValueTracking.h"

#include "Luau/Lexer.h"
#include "Luau/SmallVector.h"

LUAU_FASTFLAG(LuauOptimizeExportTable)
LUAU_FASTFLAG(LuauCompileReuseLocalRegs)

namespace Luau
{
namespace Compile
{

struct ValueVisitor : AstVisitor
{

    DenseHashMap<AstName, Global>& globals;
    DenseHashMap<AstLocal*, Variable>& variables;
    DenseHashSet<AstLocal*>* exportedFunctions = nullptr;
    std::vector<AstLocal*>* exportedVariables = nullptr;
    SmallVector<AstStatBlock*, 4> blockOwnerStack;

    // with LuauOptimizeExportTable, remove this constructor
    ValueVisitor(DenseHashMap<AstName, Global>& globals, DenseHashMap<AstLocal*, Variable>& variables)
        : globals(globals)
        , variables(variables)
    {
    }

    ValueVisitor(
        DenseHashMap<AstName, Global>& globals,
        DenseHashMap<AstLocal*, Variable>& variables,
        DenseHashSet<AstLocal*>* exportedFunctions,
        std::vector<AstLocal*>* exportedVariables
    )
        : globals(globals)
        , variables(variables)
        , exportedFunctions(exportedFunctions)
        , exportedVariables(exportedVariables)
    {
    }

    void assign(AstExpr* var)
    {
        if (AstExprLocal* lv = var->as<AstExprLocal>())
        {
            if (FFlag::LuauCompileReuseLocalRegs)
            {
                Variable& variable = variables[lv->local];

                variable.written = true;
                variable.lastUsed = lv;

                if (lv->upvalue || lv->local->isExported)
                    variable.nonLexicalUse = true;
            }
            else
            {
                variables[lv->local].written = true;
            }
        }
        else if (AstExprGlobal* gv = var->as<AstExprGlobal>())
        {
            globals[gv->name] = Global::Written;
        }
        else
        {
            // we need to be able to track assignments in all expressions, including crazy ones like t[function() t = nil end] = 5
            var->visit(this);
        }
    }

    bool visit(AstExprLocal* node) override
    {
        if (FFlag::LuauCompileReuseLocalRegs)
        {
            Variable& variable = variables[node->local];

            variable.lastUsed = node;

            if (node->upvalue || node->local->isExported)
                variable.nonLexicalUse = true;

            return false;
        }
        else
        {
            return true;
        }
    }

    bool visit(AstStatLocal* node) override
    {
        for (size_t i = 0; i < node->vars.size && i < node->values.size; ++i)
            variables[node->vars.data[i]].init = node->values.data[i];

        for (size_t i = node->values.size; i < node->vars.size; ++i)
            variables[node->vars.data[i]].init = nullptr;

        if (FFlag::LuauOptimizeExportTable && exportedVariables)
        {
            for (size_t i = 0; i < node->vars.size; ++i)
            {
                AstLocal* local = node->vars.data[i];
                if (local->isExported)
                {
                    exportedVariables->push_back(local);
                }
            }
        }

        if (FFlag::LuauCompileReuseLocalRegs)
        {
            for (size_t i = 0; i < node->vars.size; ++i)
            {
                AstLocal* local = node->vars.data[i];

                LUAU_ASSERT(!blockOwnerStack.empty());
                variables[local].owner = blockOwnerStack.back();
            }
        }

        return true;
    }

    bool visit(AstStatIf* node) override
    {
        if (node->conditionLocal)
            variables[node->conditionLocal].init = node->condition;

        return true;
    }

    bool visit(AstExprIfElse* node) override
    {
        if (node->conditionLocal)
            variables[node->conditionLocal].init = node->condition;

        return true;
    }

    bool visit(AstStatAssign* node) override
    {
        for (size_t i = 0; i < node->vars.size; ++i)
            assign(node->vars.data[i]);

        for (size_t i = 0; i < node->values.size; ++i)
            node->values.data[i]->visit(this);

        return false;
    }

    bool visit(AstStatCompoundAssign* node) override
    {
        assign(node->var);
        node->value->visit(this);

        return false;
    }

    bool visit(AstStatLocalFunction* node) override
    {
        if (FFlag::LuauCompileReuseLocalRegs)
        {
            Variable& variable = variables[node->name];

            variable.init = node->func;

            LUAU_ASSERT(!blockOwnerStack.empty());
            variable.owner = blockOwnerStack.back();

            if (node->name->isExported)
                variable.nonLexicalUse = true;
        }
        else
        {
            variables[node->name].init = node->func;
        }

        if (FFlag::LuauOptimizeExportTable && exportedFunctions && node->name->isExported)
        {
            exportedFunctions->insert(node->name);
            exportedVariables->push_back(node->name);
        }

        return true;
    }

    bool visit(AstStatFunction* node) override
    {
        assign(node->name);
        node->func->visit(this);

        return false;
    }

    bool visit(AstExprFunction* node) override
    {
        for (AstLocal* arg : node->args)
            variables[arg].init = nullptr;

        return true;
    }

    bool visit(AstStatBlock* block) override
    {
        if (FFlag::LuauCompileReuseLocalRegs)
        {
            blockOwnerStack.push_back(block);
            for (AstStat* stat : block->body)
                stat->visit(this);
            blockOwnerStack.pop_back();
            return false;
        }
        else
        {
            return true;
        }
    }
};

void assignMutable(DenseHashMap<AstName, Global>& globals, const AstNameTable& names, const char* const* mutableGlobals)
{
    if (AstName name = names.get("_G"); name.value)
        globals[name] = Global::Mutable;

    if (mutableGlobals)
        for (const char* const* ptr = mutableGlobals; *ptr; ++ptr)
            if (AstName name = names.get(*ptr); name.value)
                globals[name] = Global::Mutable;
}

void trackValues(
    DenseHashMap<AstName, Global>& globals,
    DenseHashMap<AstLocal*, Variable>& variables,
    DenseHashSet<AstLocal*>& exportedFunctions,
    std::vector<AstLocal*>& exportedVariables,
    AstNode* root
)
{
    ValueVisitor visitor{globals, variables, &exportedFunctions, &exportedVariables};
    root->visit(&visitor);
}
void trackValues_DEPRECATED(DenseHashMap<AstName, Global>& globals, DenseHashMap<AstLocal*, Variable>& variables, AstNode* root)
{
    ValueVisitor visitor{globals, variables};
    root->visit(&visitor);
}

} // namespace Compile
} // namespace Luau
