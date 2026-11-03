// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "TableShape.h"

namespace Luau
{
namespace Compile
{

// conservative limit for the loop bound that establishes table array size
static const int kMaxLoopBound = 16;

static AstExprTable* getTableHint(AstExpr* expr)
{
    // unadorned table literal
    if (AstExprTable* table = expr->as<AstExprTable>())
        return table;

    // setmetatable(table literal, ...)
    if (AstExprCall* call = expr->as<AstExprCall>(); call && !call->self && call->args.size == 2)
        if (AstExprGlobal* func = call->func->as<AstExprGlobal>(); func && func->name == "setmetatable")
            if (AstExprTable* table = call->args.data[0]->as<AstExprTable>())
                return table;

    return nullptr;
}

struct ShapeVisitor : AstVisitor
{
    struct Hasher
    {
        size_t operator()(const std::pair<AstExprTable*, AstName>& p) const
        {
            return DenseHashPointer()(p.first) ^ std::hash<AstName>()(p.second);
        }
    };

    DenseHashMap<AstExprTable*, TableShape>& shapes;

    DenseHashMap<AstLocal*, AstExprTable*> tables;
    DenseHashSet<std::pair<AstExprTable*, AstName>, Hasher> fields;

    DenseHashMap<AstLocal*, unsigned int> loops; // iterator => upper bound for 1..k

    ShapeVisitor(DenseHashMap<AstExprTable*, TableShape>& shapes)
        : shapes(shapes)
    {
    }

    void assignField(AstExpr* expr, AstName index)
    {
        if (AstExprLocal* lv = expr->as<AstExprLocal>())
        {
            if (AstExprTable** table = tables.find(lv->local))
            {
                std::pair<AstExprTable*, AstName> field = {*table, index};

                if (!fields.contains(field))
                {
                    fields.insert(field);
                    shapes[*table].hashSize += 1;
                }
            }
        }
    }

    void assignField(AstExpr* expr, AstExpr* index)
    {
        AstExprLocal* lv = expr->as<AstExprLocal>();
        if (!lv)
            return;

        AstExprTable** table = tables.find(lv->local);
        if (!table)
            return;

        if (AstExprConstantNumber* number = index->as<AstExprConstantNumber>())
        {
            TableShape& shape = shapes[*table];

            if (number->value == double(shape.arraySize + 1))
                shape.arraySize += 1;
        }
        else if (AstExprLocal* iter = index->as<AstExprLocal>())
        {
            if (const unsigned int* bound = loops.find(iter->local))
            {
                TableShape& shape = shapes[*table];

                if (shape.arraySize == 0)
                    shape.arraySize = *bound;
            }
        }
    }

    void assign(AstExpr* var)
    {
        if (AstExprIndexName* index = var->as<AstExprIndexName>())
        {
            assignField(index->expr, index->index);
        }
        else if (AstExprIndexExpr* index = var->as<AstExprIndexExpr>())
        {
            assignField(index->expr, index->index);
        }
    }

    bool visit(AstStatLocal* node) override
    {
        // track local -> table association so that we can update table size prediction in assignField
        if (node->vars.size == 1 && node->values.size == 1)
            if (AstExprTable* table = getTableHint(node->values.data[0]); table && table->items.size == 0)
                tables[node->vars.data[0]] = table;

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

    bool visit(AstStatFunction* node) override
    {
        assign(node->name);
        node->func->visit(this);

        return false;
    }

    bool visit(AstStatFor* node) override
    {
        AstExprConstantNumber* from = node->from->as<AstExprConstantNumber>();
        AstExprConstantNumber* to = node->to->as<AstExprConstantNumber>();

        if (from && to && from->value == 1.0 && to->value >= 1.0 && to->value <= double(kMaxLoopBound) && !node->step)
            loops[node->var] = unsigned(to->value);

        return true;
    }
};

void predictTableShapes(DenseHashMap<AstExprTable*, TableShape>& shapes, AstNode* root)
{
    ShapeVisitor visitor{shapes};
    root->visit(&visitor);
}

} // namespace Compile
} // namespace Luau
