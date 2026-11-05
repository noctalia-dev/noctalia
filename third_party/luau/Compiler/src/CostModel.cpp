// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "CostModel.h"

#include "Luau/Bytecode.h"
#include "Luau/Common.h"
#include "Luau/DenseHash.h"

#include "ConstantFolding.h"
#include "Utils.h"

#include <limits.h>

namespace Luau
{
namespace Compile
{

inline uint64_t parallelAddSat(uint64_t x, uint64_t y)
{
    uint64_t r = x + y;
    uint64_t s = r & 0x8080808080808080ull; // saturation mask

    return (r ^ s) | (s - (s >> 7));
}

static uint64_t parallelMulSat(uint64_t a, int b)
{
    int bs = (b < 127) ? b : 127;

    // multiply every other value by b, yielding 14-bit products
    uint64_t l = bs * ((a >> 0) & 0x007f007f007f007full);
    uint64_t h = bs * ((a >> 8) & 0x007f007f007f007full);

    // each product is 14-bit, so adding 32768-128 sets high bit iff the sum is 128 or larger without an overflow
    uint64_t ls = l + 0x7f807f807f807f80ull;
    uint64_t hs = h + 0x7f807f807f807f80ull;

    // we now merge saturation bits as well as low 7-bits of each product into one
    uint64_t s = (hs & 0x8000800080008000ull) | ((ls & 0x8000800080008000ull) >> 8);
    uint64_t r = ((h & 0x007f007f007f007full) << 8) | (l & 0x007f007f007f007full);

    // the low bits are now correct for values that didn't saturate, and we simply need to mask them if high bit is 1
    return r | (s - (s >> 7));
}

struct Cost
{
    static const uint64_t kLiteral = ~0ull;

    // cost model: 8 bytes, where first byte is the baseline cost, and the next 7 bytes are discounts for when variable #i is constant
    uint64_t model;
    // constant mask: 8-byte 0xff mask; equal to all ff's for literals, for variables only byte #i (1+) is set to align with model
    uint64_t constant;

    Cost(int cost = 0, uint64_t constant = 0)
        : model(cost < 0x7f ? cost : 0x7f)
        , constant(constant)
    {
    }

    Cost operator+(const Cost& other) const
    {
        Cost result;
        result.model = parallelAddSat(model, other.model);
        return result;
    }

    Cost& operator+=(const Cost& other)
    {
        model = parallelAddSat(model, other.model);
        constant = 0;
        return *this;
    }

    Cost operator*(int other) const
    {
        Cost result;
        result.model = parallelMulSat(model, other);
        return result;
    }

    static Cost fold(const Cost& x, const Cost& y)
    {
        uint64_t newmodel = parallelAddSat(x.model, y.model);
        uint64_t newconstant = x.constant & y.constant;

        // the extra cost for folding is 1; the discount is 1 for the variable that is shared by x&y (or whichever one is used in x/y if the other is
        // literal)
        uint64_t extra = (newconstant == kLiteral) ? 0 : (1 | (0x0101010101010101ull & newconstant));

        Cost result;
        result.model = parallelAddSat(newmodel, extra);
        result.constant = newconstant;

        return result;
    }
};

struct CostVisitor : AstVisitor
{
    const DenseHashMap<AstExprCall*, int>& builtins;
    const DenseHashMap<AstExpr*, Constant>& constants;

    DenseHashMap<AstLocal*, uint64_t> vars;
    Cost result;

    CostVisitor(const DenseHashMap<AstExprCall*, int>& builtins, const DenseHashMap<AstExpr*, Constant>& constants)
        : builtins(builtins)
        , constants(constants)
    {
    }

    Cost model(AstExpr* node)
    {
        if (const Constant* c = constants.find(node))
            return Cost(0, Cost::kLiteral);

        if (AstExprGroup* expr = node->as<AstExprGroup>())
        {
            return model(expr->expr);
        }
        else if (node->is<AstExprConstantNil>() || node->is<AstExprConstantBool>() || node->is<AstExprConstantNumber>() ||
                 node->is<AstExprConstantString>() || node->is<AstExprConstantInteger>())
        {
            return Cost(0, Cost::kLiteral);
        }
        else if (AstExprLocal* expr = node->as<AstExprLocal>())
        {
            const uint64_t* i = vars.find(expr->local);

            return Cost(0, i ? *i : 0); // locals typically don't require extra instructions to compute
        }
        else if (node->is<AstExprGlobal>())
        {
            return 1;
        }
        else if (node->is<AstExprVarargs>())
        {
            return 3;
        }
        else if (AstExprCall* expr = node->as<AstExprCall>())
        {
            // builtin cost modeling is different from regular calls because we use FASTCALL to compile these
            // thus we use a cheaper baseline, don't account for function, and assume constant/local copy is free
            const int* bfid = builtins.find(expr);
            bool builtin = bfid != nullptr && *bfid != LBF_NONE;
            bool builtinShort = builtin && expr->args.size <= 3u; // FASTCALL1/2/3

            Cost cost = builtin ? 2 : 3;

            if (!builtin)
                cost += model(expr->func);

            for (size_t i = 0; i < expr->args.size; ++i)
            {
                Cost ac = model(expr->args.data[i]);
                // for constants/locals we still need to copy them to the argument list
                cost += ac.model == 0 && !builtinShort ? Cost(1) : ac;
            }

            return cost;
        }
        else if (AstExprIndexName* expr = node->as<AstExprIndexName>())
        {
            return model(expr->expr) + 1;
        }
        else if (AstExprIndexExpr* expr = node->as<AstExprIndexExpr>())
        {
            return model(expr->expr) + model(expr->index) + 1;
        }
        else if (AstExprFunction* expr = node->as<AstExprFunction>())
        {
            return 10; // high baseline cost due to allocation
        }
        else if (AstExprTable* expr = node->as<AstExprTable>())
        {
            Cost cost = 10; // high baseline cost due to allocation

            for (size_t i = 0; i < expr->items.size; ++i)
            {
                const AstExprTable::Item& item = expr->items.data[i];

                if (item.key)
                    cost += model(item.key);

                cost += model(item.value);
                cost += 1;
            }

            return cost;
        }
        else if (AstExprUnary* expr = node->as<AstExprUnary>())
        {
            return Cost::fold(model(expr->expr), Cost(0, Cost::kLiteral));
        }
        else if (AstExprBinary* expr = node->as<AstExprBinary>())
        {
            return Cost::fold(model(expr->left), model(expr->right));
        }
        else if (AstExprTypeAssertion* expr = node->as<AstExprTypeAssertion>())
        {
            return model(expr->expr);
        }
        else if (AstExprIfElse* expr = node->as<AstExprIfElse>())
        {
            Cost cond = model(expr->condition);

            // propagate constant mask from condition to the local variable if it exists
            if (expr->conditionLocal && cond.constant != 0)
                vars[expr->conditionLocal] = cond.constant;

            return cond + model(expr->trueExpr) + model(expr->falseExpr) + 2;
        }
        else if (AstExprInterpString* expr = node->as<AstExprInterpString>())
        {
            // Baseline cost of string.format
            Cost cost = 3;

            for (AstExpr* innerExpression : expr->expressions)
                cost += model(innerExpression);

            return cost;
        }
        else if (AstExprInstantiate* expr = node->as<AstExprInstantiate>())
        {
            return model(expr->expr);
        }
        else
        {
            LUAU_ASSERT(!"Unknown expression type");
            return {};
        }
    }

    void assign(AstExpr* expr)
    {
        // variable assignments reset variable mask, so that further uses of this variable aren't discounted
        // this doesn't work perfectly with backwards control flow like loops, but is good enough for a single pass
        if (AstExprLocal* lv = expr->as<AstExprLocal>())
            if (uint64_t* i = vars.find(lv->local))
                *i = 0;
    }

    void loop(AstStatBlock* body, Cost iterCost, int factor = 3)
    {
        Cost before = result;

        result = Cost();
        body->visit(this);

        result = before + (result + iterCost) * factor;
    }

    bool visit(AstExpr* node) override
    {
        // note: we short-circuit the visitor traversal through any expression trees by returning false
        // recursive traversal is happening inside model() which makes it easier to get the resulting value of the subexpression
        result += model(node);

        return false;
    }

    bool visit(AstStatFor* node) override
    {
        result += model(node->from);
        result += model(node->to);

        if (node->step)
            result += model(node->step);

        int tripCount = -1;
        double from, to, step = 1;

        if (getNumber(node->from, from) && getNumber(node->to, to) && (!node->step || getNumber(node->step, step)))
            tripCount = getTripCount(from, to, step);

        loop(node->body, 1, tripCount < 0 ? 3 : tripCount);
        return false;
    }

    bool visit(AstStatForIn* node) override
    {
        for (size_t i = 0; i < node->values.size; ++i)
            result += model(node->values.data[i]);

        loop(node->body, 1);
        return false;
    }

    bool visit(AstStatWhile* node) override
    {
        Cost condition = model(node->condition);

        loop(node->body, condition);
        return false;
    }

    bool visit(AstStatRepeat* node) override
    {
        Cost condition = model(node->condition);

        loop(node->body, condition);
        return false;
    }

    bool visit(AstStatIf* node) override
    {
        if (node->conditionLocal)
        {
            Cost arg = model(node->condition);

            if (arg.constant != 0)
                vars[node->conditionLocal] = arg.constant;
        }

        if (isConstantFalse(constants, node->condition))
        {
            if (node->elsebody)
                node->elsebody->visit(this);
            return false;
        }

        if (isConstantTrue(constants, node->condition))
        {
            node->thenbody->visit(this);
            return false;
        }

        // unconditional 'else' may require a jump after the 'if' body
        // note: this ignores cases when 'then' always terminates and also assumes comparison requires an extra instruction which may be false
        result += 1 + (node->elsebody && !node->elsebody->is<AstStatIf>());

        return true;
    }

    bool visit(AstStatLocal* node) override
    {
        for (size_t i = 0; i < node->values.size; ++i)
        {
            Cost arg = model(node->values.data[i]);

            // propagate constant mask from expression through variables
            if (arg.constant && i < node->vars.size)
                vars[node->vars.data[i]] = arg.constant;

            result += arg;
        }

        return false;
    }

    bool visit(AstStatAssign* node) override
    {
        for (size_t i = 0; i < node->vars.size; ++i)
            assign(node->vars.data[i]);

        for (size_t i = 0; i < node->vars.size || i < node->values.size; ++i)
        {
            Cost ac;
            if (i < node->vars.size)
                ac += model(node->vars.data[i]);
            if (i < node->values.size)
                ac += model(node->values.data[i]);
            // local->local or constant->local assignment is not free
            result += ac.model == 0 ? Cost(1) : ac;
        }

        return false;
    }

    bool visit(AstStatCompoundAssign* node) override
    {
        assign(node->var);

        // if lhs is not a local, setting it requires an extra table operation
        result += node->var->is<AstExprLocal>() ? 1 : 2;

        return true;
    }

    bool visit(AstStatBreak* node) override
    {
        result += 1;

        return false;
    }

    bool visit(AstStatContinue* node) override
    {
        result += 1;

        return false;
    }

    bool getNumber(AstExpr* node, double& result)
    {
        if (const Constant* constant = constants.find(node))
        {
            if (constant->type == Constant::Type_Number)
            {
                result = constant->valueNumber;
                return true;
            }
        }

        return false;
    }

    bool visit(AstStatBlock* node) override
    {
        for (size_t i = 0; i < node->body.size; ++i)
        {
            AstStat* stat = node->body.data[i];

            stat->visit(this);

            if (alwaysTerminates(constants, stat))
                break;
        }

        return false;
    }
};

uint64_t modelCost(
    AstNode* root,
    AstLocal* const* vars,
    size_t varCount,
    const DenseHashMap<AstExprCall*, int>& builtins,
    const DenseHashMap<AstExpr*, Constant>& constants
)
{
    CostVisitor visitor{builtins, constants};
    for (size_t i = 0; i < varCount && i < 7; ++i)
        visitor.vars[vars[i]] = 0xffull << (i * 8 + 8);

    root->visit(&visitor);

    return visitor.result.model;
}

uint64_t modelCost(AstNode* root, AstLocal* const* vars, size_t varCount)
{
    DenseHashMap<AstExprCall*, int> builtins;
    DenseHashMap<AstExpr*, Constant> constants;

    return modelCost(root, vars, varCount, builtins, constants);
}

int computeCost(uint64_t model, const bool* varsConst, size_t varCount)
{
    int cost = int(model & 0x7f);

    // don't apply discounts to what is likely a saturated sum
    if (cost == 0x7f)
        return cost;

    for (size_t i = 0; i < varCount && i < 7; ++i)
        cost -= int((model >> (i * 8 + 8)) & 0x7f) * varsConst[i];

    return cost;
}

int getTripCount(double from, double to, double step)
{
    // we compute trip count in integers because that way we know that the loop math (repeated addition) is precise
    int fromi = (from >= -32767 && from <= 32767 && double(int(from)) == from) ? int(from) : INT_MIN;
    int toi = (to >= -32767 && to <= 32767 && double(int(to)) == to) ? int(to) : INT_MIN;
    int stepi = (step >= -32767 && step <= 32767 && double(int(step)) == step) ? int(step) : INT_MIN;

    if (fromi == INT_MIN || toi == INT_MIN || stepi == INT_MIN || stepi == 0)
        return -1;

    if ((stepi < 0 && toi > fromi) || (stepi > 0 && toi < fromi))
        return 0;

    return (toi - fromi) / stepi + 1;
}

} // namespace Compile
} // namespace Luau
