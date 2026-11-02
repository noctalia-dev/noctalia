// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/Ast.h"

#include "Luau/Common.h"

LUAU_FASTFLAG(LuauExperimentalIfLocalSyntax)

namespace Luau
{

static AstAttr* findAttributeInArray(const AstArray<AstAttr*> attributes, AstAttr::Type attributeType)
{
    for (const auto attribute : attributes)
    {
        if (attribute->type == attributeType)
            return attribute;
    }

    return nullptr;
}

static bool hasAttributeInArray(const AstArray<AstAttr*> attributes, AstAttr::Type attributeType)
{
    return findAttributeInArray(attributes, attributeType) != nullptr;
}

static void visitTypeList(AstVisitor* visitor, const AstTypeList& list)
{
    for (AstType* ty : list.types)
        ty->visit(visitor);

    if (list.tailType)
        list.tailType->visit(visitor);
}

static void visitTypeOrPackArray(AstVisitor* visitor, const AstArray<AstTypeOrPack>& arrayOfTypeOrPack)
{
    for (const AstTypeOrPack& param : arrayOfTypeOrPack)
    {
        if (param.type)
            param.type->visit(visitor);
        else
            param.typePack->visit(visitor);
    }
}

AstAttr::AstAttr(const Location& location, Type type, AstArray<AstExpr*> args)
    : AstNode(ClassIndex(), location)
    , type(type)
    , args(args)
{
}

AstAttr::AstAttr(const Location& location, Type type, AstArray<AstExpr*> args, AstName name)
    : AstNode(ClassIndex(), location)
    , type(type)
    , args(args)
    , name(name)
{
}

void AstAttr::visit(AstVisitor* visitor)
{
    visitor->visit(this);
}

AstAttr::DeprecatedInfo AstAttr::deprecatedInfo() const
{
    AstAttr::DeprecatedInfo info;
    info.deprecated = type == AstAttr::Type::Deprecated;

    if (info.deprecated && args.size > 0 && args.data[0]->is<AstExprTable>())
    {
        AstExprTable* table = args.data[0]->as<AstExprTable>();
        if (auto useValue = table->getRecord("use"))
        {
            if ((*useValue)->is<AstExprConstantString>())
            {
                AstArray<char> use = (*useValue)->as<AstExprConstantString>()->value;
                info.use = {{use.data, use.size}};
            }
        }
        if (auto reasonValue = table->getRecord("reason"))
        {
            if ((*reasonValue)->is<AstExprConstantString>())
            {
                AstArray<char> reason = (*reasonValue)->as<AstExprConstantString>()->value;
                info.reason = {{reason.data, reason.size}};
            }
        }
    }

    return info;
}

int gAstRttiIndex = 0;

AstGenericType::AstGenericType(const Location& location, AstName name, AstType* defaultValue)
    : AstNode(ClassIndex(), location)
    , name(name)
    , defaultValue(defaultValue)
{
}

void AstGenericType::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        if (defaultValue)
            defaultValue->visit(visitor);
    }
}

AstGenericTypePack::AstGenericTypePack(const Location& location, AstName name, AstTypePack* defaultValue)
    : AstNode(ClassIndex(), location)
    , name(name)
    , defaultValue(defaultValue)
{
}

void AstGenericTypePack::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        if (defaultValue)
            defaultValue->visit(visitor);
    }
}

AstExprGroup::AstExprGroup(const Location& location, AstExpr* expr)
    : AstExpr(ClassIndex(), location)
    , expr(expr)
{
}

void AstExprGroup::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
        expr->visit(visitor);
}

AstExprConstantNil::AstExprConstantNil(const Location& location)
    : AstExpr(ClassIndex(), location)
{
}

void AstExprConstantNil::visit(AstVisitor* visitor)
{
    visitor->visit(this);
}

AstExprConstantBool::AstExprConstantBool(const Location& location, bool value)
    : AstExpr(ClassIndex(), location)
    , value(value)
{
}

void AstExprConstantBool::visit(AstVisitor* visitor)
{
    visitor->visit(this);
}

AstExprConstantNumber::AstExprConstantNumber(const Location& location, double value, ConstantNumberParseResult parseResult)
    : AstExpr(ClassIndex(), location)
    , value(value)
    , parseResult(parseResult)
{
}

void AstExprConstantNumber::visit(AstVisitor* visitor)
{
    visitor->visit(this);
}

AstExprConstantInteger::AstExprConstantInteger(const Location& location, int64_t value, ConstantNumberParseResult parseResult)
    : AstExpr(ClassIndex(), location)
    , value(value)
    , parseResult(parseResult)
{
}

void AstExprConstantInteger::visit(AstVisitor* visitor)
{
    visitor->visit(this);
}

AstExprConstantString::AstExprConstantString(const Location& location, const AstArray<char>& value, QuoteStyle quoteStyle)
    : AstExpr(ClassIndex(), location)
    , value(value)
    , quoteStyle(quoteStyle)
{
}

void AstExprConstantString::visit(AstVisitor* visitor)
{
    visitor->visit(this);
}

bool AstExprConstantString::isQuoted() const
{
    return quoteStyle == QuoteStyle::QuotedSimple || quoteStyle == QuoteStyle::QuotedRaw;
}

AstExprLocal::AstExprLocal(const Location& location, AstLocal* local, bool upvalue)
    : AstExpr(ClassIndex(), location)
    , local(local)
    , upvalue(upvalue)
{
}

void AstExprLocal::visit(AstVisitor* visitor)
{
    visitor->visit(this);
}

AstExprGlobal::AstExprGlobal(const Location& location, const AstName& name)
    : AstExpr(ClassIndex(), location)
    , name(name)
{
}

void AstExprGlobal::visit(AstVisitor* visitor)
{
    visitor->visit(this);
}

AstExprVarargs::AstExprVarargs(const Location& location)
    : AstExpr(ClassIndex(), location)
{
}

void AstExprVarargs::visit(AstVisitor* visitor)
{
    visitor->visit(this);
}

AstExprCall::AstExprCall(
    const Location& location,
    AstExpr* func,
    const AstArray<AstExpr*>& args,
    bool self,
    bool tableCall,
    const AstArray<AstTypeOrPack>& explicitTypes,
    const Location& argLocation
)
    : AstExpr(ClassIndex(), location)
    , func(func)
    , typeArguments(explicitTypes)
    , args(args)
    , self(self)
    , tableCall(tableCall)
    , argLocation(argLocation)
{
}

void AstExprCall::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        func->visit(visitor);

        for (AstExpr* arg : args)
            arg->visit(visitor);
    }
}

AstExprIndexName::AstExprIndexName(
    const Location& location,
    AstExpr* expr,
    const AstName& index,
    const Location& indexLocation,
    const Position& opPosition,
    char op
)
    : AstExpr(ClassIndex(), location)
    , expr(expr)
    , index(index)
    , indexLocation(indexLocation)
    , opPosition(opPosition)
    , op(op)
{
}

void AstExprIndexName::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
        expr->visit(visitor);
}

AstExprIndexExpr::AstExprIndexExpr(const Location& location, AstExpr* expr, AstExpr* index)
    : AstExpr(ClassIndex(), location)
    , expr(expr)
    , index(index)
{
}

void AstExprIndexExpr::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        expr->visit(visitor);
        index->visit(visitor);
    }
}

AstExprFunction::AstExprFunction(
    const Location& location,
    const AstArray<AstAttr*>& attributes,
    const AstArray<AstGenericType*>& generics,
    const AstArray<AstGenericTypePack*>& genericPacks,
    AstLocal* self,
    const AstArray<AstLocal*>& args,
    bool vararg,
    const Location& varargLocation,
    AstStatBlock* body,
    size_t functionDepth,
    const AstName& debugname,
    AstTypePack* returnAnnotation,
    AstTypePack* varargAnnotation,
    const std::optional<Location>& argLocation
)
    : AstExpr(ClassIndex(), location)
    , attributes(attributes)
    , generics(generics)
    , genericPacks(genericPacks)
    , self(self)
    , args(args)
    , returnAnnotation(returnAnnotation)
    , vararg(vararg)
    , varargLocation(varargLocation)
    , varargAnnotation(varargAnnotation)
    , body(body)
    , functionDepth(functionDepth)
    , debugname(debugname)
    , argLocation(argLocation)
{
}

void AstExprFunction::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        for (AstLocal* arg : args)
        {
            if (arg->annotation)
                arg->annotation->visit(visitor);
        }

        if (varargAnnotation)
            varargAnnotation->visit(visitor);

        if (returnAnnotation)
            returnAnnotation->visit(visitor);

        body->visit(visitor);
    }
}

bool AstExprFunction::hasNativeAttribute() const
{
    for (const auto attribute : attributes)
    {
        if (attribute->type == AstAttr::Type::Native)
            return true;
    }
    return false;
}

bool AstExprFunction::hasAttribute(const AstAttr::Type attributeType) const
{
    return hasAttributeInArray(attributes, attributeType);
}

AstAttr* AstExprFunction::getAttribute(const AstAttr::Type attributeType) const
{
    return findAttributeInArray(attributes, attributeType);
}

AstExprTable::AstExprTable(const Location& location, const AstArray<Item>& items)
    : AstExpr(ClassIndex(), location)
    , items(items)
{
}

void AstExprTable::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        for (const Item& item : items)
        {
            if (item.key)
                item.key->visit(visitor);

            item.value->visit(visitor);
        }
    }
}

std::optional<AstExpr*> AstExprTable::getRecord(const char* key) const
{
    for (const AstExprTable::Item& item : items)
    {
        if (item.kind == AstExprTable::Item::Kind::Record)
        {
            if (strcmp(item.key->as<AstExprConstantString>()->value.data, key) == 0)
                return item.value;
        }
    }
    return {};
}

AstExprUnary::AstExprUnary(const Location& location, Op op, AstExpr* expr)
    : AstExpr(ClassIndex(), location)
    , op(op)
    , expr(expr)
{
}

void AstExprUnary::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
        expr->visit(visitor);
}

std::string toString(AstExprUnary::Op op)
{
    switch (op)
    {
    case AstExprUnary::Op::Minus:
        return "-";
    case AstExprUnary::Op::Not:
        return "not";
    case AstExprUnary::Op::Len:
        return "#";
    default:
        LUAU_ASSERT(false);
        return ""; // MSVC requires this even though the switch/case is exhaustive
    }
}

AstExprBinary::AstExprBinary(const Location& location, Op op, AstExpr* left, AstExpr* right)
    : AstExpr(ClassIndex(), location)
    , op(op)
    , left(left)
    , right(right)
{
}

void AstExprBinary::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        left->visit(visitor);
        right->visit(visitor);
    }
}

std::string toString(AstExprBinary::Op op)
{
    switch (op)
    {
    case AstExprBinary::Add:
        return "+";
    case AstExprBinary::Sub:
        return "-";
    case AstExprBinary::Mul:
        return "*";
    case AstExprBinary::Div:
        return "/";
    case AstExprBinary::FloorDiv:
        return "//";
    case AstExprBinary::Mod:
        return "%";
    case AstExprBinary::Pow:
        return "^";
    case AstExprBinary::Concat:
        return "..";
    case AstExprBinary::CompareNe:
        return "~=";
    case AstExprBinary::CompareEq:
        return "==";
    case AstExprBinary::CompareLt:
        return "<";
    case AstExprBinary::CompareLe:
        return "<=";
    case AstExprBinary::CompareGt:
        return ">";
    case AstExprBinary::CompareGe:
        return ">=";
    case AstExprBinary::And:
        return "and";
    case AstExprBinary::Or:
        return "or";
    default:
        LUAU_ASSERT(false);
        return ""; // MSVC requires this even though the switch/case is exhaustive
    }
}

AstExprTypeAssertion::AstExprTypeAssertion(const Location& location, AstExpr* expr, AstType* annotation)
    : AstExpr(ClassIndex(), location)
    , expr(expr)
    , annotation(annotation)
{
}

void AstExprTypeAssertion::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        expr->visit(visitor);
        annotation->visit(visitor);
    }
}

AstExprIfElse::AstExprIfElse(const Location& location, AstExpr* condition, bool hasThen, AstExpr* trueExpr, bool hasElse, AstExpr* falseExpr)
    : AstExpr(ClassIndex(), location)
    , condition(condition)
    , hasThen(hasThen)
    , trueExpr(trueExpr)
    , hasElse(hasElse)
    , falseExpr(falseExpr)
{
}

AstExprIfElse::AstExprIfElse(
    const Location& location,
    AstExpr* condition,
    bool hasThen,
    AstExpr* trueExpr,
    bool hasElse,
    AstExpr* falseExpr,
    AstLocal* conditionLocal,
    bool conditionIsConst,
    const std::optional<Location>& conditionKeywordLocation,
    const std::optional<Location>& conditionEqualsLocation
)
    : AstExpr(ClassIndex(), location)
    , condition(condition)
    , hasThen(hasThen)
    , trueExpr(trueExpr)
    , hasElse(hasElse)
    , falseExpr(falseExpr)
    , conditionLocal(conditionLocal)
    , conditionIsConst(conditionIsConst)
    , conditionKeywordLocation(conditionKeywordLocation)
    , conditionEqualsLocation(conditionEqualsLocation)
{
}

void AstExprIfElse::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        if (FFlag::LuauExperimentalIfLocalSyntax && conditionLocal && conditionLocal->annotation)
            conditionLocal->annotation->visit(visitor);

        condition->visit(visitor);
        trueExpr->visit(visitor);
        falseExpr->visit(visitor);
    }
}

AstExprError::AstExprError(const Location& location, const AstArray<AstExpr*>& expressions, unsigned messageIndex)
    : AstExpr(ClassIndex(), location)
    , expressions(expressions)
    , messageIndex(messageIndex)
{
}

AstExprInterpString::AstExprInterpString(const Location& location, const AstArray<AstArray<char>>& strings, const AstArray<AstExpr*>& expressions)
    : AstExpr(ClassIndex(), location)
    , strings(strings)
    , expressions(expressions)
{
}

void AstExprInterpString::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        for (AstExpr* expr : expressions)
            expr->visit(visitor);
    }
}

AstExprInstantiate::AstExprInstantiate(const Location& location, AstExpr* expr, AstArray<AstTypeOrPack> types)
    : AstExpr(ClassIndex(), location)
    , expr(expr)
    , typeArguments(types)
{
}

void AstExprInstantiate::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        expr->visit(visitor);
        visitTypeOrPackArray(visitor, typeArguments);
    }
}


void AstExprError::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        for (AstExpr* expression : expressions)
            expression->visit(visitor);
    }
}

AstStatBlock::AstStatBlock(const Location& location, const AstArray<AstStat*>& body, bool hasEnd)
    : AstStat(ClassIndex(), location)
    , body(body)
    , hasEnd(hasEnd)
{
}

void AstStatBlock::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        for (AstStat* stat : body)
            stat->visit(visitor);
    }
}

AstStatIf::AstStatIf(
    const Location& location,
    AstExpr* condition,
    AstStatBlock* thenbody,
    AstStat* elsebody,
    const std::optional<Location>& thenLocation,
    const std::optional<Location>& elseLocation
)
    : AstStat(ClassIndex(), location)
    , condition(condition)
    , thenbody(thenbody)
    , elsebody(elsebody)
    , thenLocation(thenLocation)
    , elseLocation(elseLocation)
{
}

AstStatIf::AstStatIf(
    const Location& location,
    AstExpr* condition,
    AstStatBlock* thenbody,
    AstStat* elsebody,
    const std::optional<Location>& thenLocation,
    const std::optional<Location>& elseLocation,
    AstLocal* conditionLocal,
    bool conditionIsConst,
    const std::optional<Location>& conditionKeywordLocation,
    const std::optional<Location>& conditionEqualsLocation
)
    : AstStat(ClassIndex(), location)
    , condition(condition)
    , thenbody(thenbody)
    , elsebody(elsebody)
    , thenLocation(thenLocation)
    , elseLocation(elseLocation)
    , conditionLocal(conditionLocal)
    , conditionIsConst(conditionIsConst)
    , conditionKeywordLocation(conditionKeywordLocation)
    , conditionEqualsLocation(conditionEqualsLocation)
{
}

void AstStatIf::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        if (FFlag::LuauExperimentalIfLocalSyntax && conditionLocal && conditionLocal->annotation)
            conditionLocal->annotation->visit(visitor);

        condition->visit(visitor);
        thenbody->visit(visitor);

        if (elsebody)
            elsebody->visit(visitor);
    }
}

AstStatWhile::AstStatWhile(const Location& location, AstExpr* condition, AstStatBlock* body, bool hasDo, const Location& doLocation)
    : AstStat(ClassIndex(), location)
    , condition(condition)
    , body(body)
    , hasDo(hasDo)
    , doLocation(doLocation)
{
}

void AstStatWhile::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        condition->visit(visitor);
        body->visit(visitor);
    }
}

AstStatRepeat::AstStatRepeat(const Location& location, AstExpr* condition, AstStatBlock* body, bool DEPRECATED_hasUntil)
    : AstStat(ClassIndex(), location)
    , condition(condition)
    , body(body)
    , DEPRECATED_hasUntil(DEPRECATED_hasUntil)
{
}

void AstStatRepeat::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        body->visit(visitor);
        condition->visit(visitor);
    }
}

AstStatBreak::AstStatBreak(const Location& location)
    : AstStat(ClassIndex(), location)
{
}

void AstStatBreak::visit(AstVisitor* visitor)
{
    visitor->visit(this);
}

AstStatContinue::AstStatContinue(const Location& location)
    : AstStat(ClassIndex(), location)
{
}

void AstStatContinue::visit(AstVisitor* visitor)
{
    visitor->visit(this);
}

AstStatReturn::AstStatReturn(const Location& location, const AstArray<AstExpr*>& list)
    : AstStat(ClassIndex(), location)
    , list(list)
{
}

void AstStatReturn::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        for (AstExpr* expr : list)
            expr->visit(visitor);
    }
}

AstStatExpr::AstStatExpr(const Location& location, AstExpr* expr)
    : AstStat(ClassIndex(), location)
    , expr(expr)
{
}

void AstStatExpr::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
        expr->visit(visitor);
}

AstStatLocal::AstStatLocal(
    const Location& location,
    const AstArray<AstLocal*>& vars,
    const AstArray<AstExpr*>& values,
    const std::optional<Location>& equalsSignLocation,
    bool isConst
)
    : AstStat(ClassIndex(), location)
    , vars(vars)
    , values(values)
    , isConst(isConst)
    , equalsSignLocation(equalsSignLocation)
{
}

void AstStatLocal::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        for (AstLocal* var : vars)
        {
            if (var->annotation)
                var->annotation->visit(visitor);
        }

        for (AstExpr* expr : values)
            expr->visit(visitor);
    }
}

AstStatFor::AstStatFor(
    const Location& location,
    AstLocal* var,
    AstExpr* from,
    AstExpr* to,
    AstExpr* step,
    AstStatBlock* body,
    bool hasDo,
    const Location& doLocation
)
    : AstStat(ClassIndex(), location)
    , var(var)
    , from(from)
    , to(to)
    , step(step)
    , body(body)
    , hasDo(hasDo)
    , doLocation(doLocation)
{
}

void AstStatFor::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        if (var->annotation)
            var->annotation->visit(visitor);

        from->visit(visitor);
        to->visit(visitor);

        if (step)
            step->visit(visitor);

        body->visit(visitor);
    }
}

AstStatForIn::AstStatForIn(
    const Location& location,
    const AstArray<AstLocal*>& vars,
    const AstArray<AstExpr*>& values,
    AstStatBlock* body,
    bool hasIn,
    const Location& inLocation,
    bool hasDo,
    const Location& doLocation
)
    : AstStat(ClassIndex(), location)
    , vars(vars)
    , values(values)
    , body(body)
    , hasIn(hasIn)
    , inLocation(inLocation)
    , hasDo(hasDo)
    , doLocation(doLocation)
{
}

void AstStatForIn::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        for (AstLocal* var : vars)
        {
            if (var->annotation)
                var->annotation->visit(visitor);
        }

        for (AstExpr* expr : values)
            expr->visit(visitor);

        body->visit(visitor);
    }
}

AstStatAssign::AstStatAssign(const Location& location, const AstArray<AstExpr*>& vars, const AstArray<AstExpr*>& values)
    : AstStat(ClassIndex(), location)
    , vars(vars)
    , values(values)
{
}

void AstStatAssign::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        for (AstExpr* lvalue : vars)
            lvalue->visit(visitor);

        for (AstExpr* expr : values)
            expr->visit(visitor);
    }
}

AstStatCompoundAssign::AstStatCompoundAssign(const Location& location, AstExprBinary::Op op, AstExpr* var, AstExpr* value)
    : AstStat(ClassIndex(), location)
    , op(op)
    , var(var)
    , value(value)
{
}

void AstStatCompoundAssign::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        var->visit(visitor);
        value->visit(visitor);
    }
}

AstStatFunction::AstStatFunction(const Location& location, AstExpr* name, AstExprFunction* func)
    : AstStat(ClassIndex(), location)
    , name(name)
    , func(func)
{
}

void AstStatFunction::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        name->visit(visitor);
        func->visit(visitor);
    }
}

AstStatLocalFunction::AstStatLocalFunction(const Location& location, AstLocal* name, AstExprFunction* func, bool isConst, Position constKeywordBegin)
    : AstStat(ClassIndex(), location)
    , name(name)
    , func(func)
    , isConst(isConst)
    , constKeywordBegin(constKeywordBegin)
{
}

void AstStatLocalFunction::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
        func->visit(visitor);
}

AstStatTypeAlias::AstStatTypeAlias(
    const Location& location,
    const AstName& name,
    const Location& nameLocation,
    const AstArray<AstGenericType*>& generics,
    const AstArray<AstGenericTypePack*>& genericPacks,
    AstType* type,
    bool exported
)
    : AstStat(ClassIndex(), location)
    , name(name)
    , nameLocation(nameLocation)
    , generics(generics)
    , genericPacks(genericPacks)
    , type(type)
    , exported(exported)
{
}

void AstStatTypeAlias::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        for (AstGenericType* el : generics)
        {
            el->visit(visitor);
        }

        for (AstGenericTypePack* el : genericPacks)
        {
            el->visit(visitor);
        }

        type->visit(visitor);
    }
}

AstStatTypeFunction::AstStatTypeFunction(
    const Location& location,
    const AstName& name,
    const Location& nameLocation,
    AstExprFunction* body,
    bool exported,
    bool hasErrors
)
    : AstStat(ClassIndex(), location)
    , name(name)
    , nameLocation(nameLocation)
    , body(body)
    , exported(exported)
    , hasErrors(hasErrors)
{
}

void AstStatTypeFunction::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
        body->visit(visitor);
}

AstStatDeclareGlobal::AstStatDeclareGlobal(const Location& location, const AstName& name, const Location& nameLocation, AstType* type)
    : AstStat(ClassIndex(), location)
    , name(name)
    , nameLocation(nameLocation)
    , type(type)
{
}

void AstStatDeclareGlobal::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
        type->visit(visitor);
}

AstStatDeclareFunction::AstStatDeclareFunction(
    const Location& location,
    const AstName& name,
    const Location& nameLocation,
    const AstArray<AstGenericType*>& generics,
    const AstArray<AstGenericTypePack*>& genericPacks,
    const AstTypeList& params,
    const AstArray<AstArgumentName>& paramNames,
    bool vararg,
    const Location& varargLocation,
    AstTypePack* retTypes
)
    : AstStat(ClassIndex(), location)
    , attributes()
    , name(name)
    , nameLocation(nameLocation)
    , generics(generics)
    , genericPacks(genericPacks)
    , params(params)
    , paramNames(paramNames)
    , vararg(vararg)
    , varargLocation(varargLocation)
    , retTypes(retTypes)
{
}

AstStatClass::AstStatClass(const Location& location, AstLocal* name, AstExpr* super, AstArray<AstClassMember> members, bool exported, bool open)
    : AstStat(ClassIndex(), location)
    , name(name)
    , super(super)
    , members(members)
    , exported(exported)
    , open(open)
{
    LUAU_ASSERT(FFlag::DebugLuauUserDefinedClasses);
}

void AstStatClass::visit(AstVisitor* visitor)
{
    LUAU_ASSERT(FFlag::DebugLuauUserDefinedClasses);
    if (visitor->visit(this))
    {
        if (super)
            super->visit(visitor);

        for (const auto& member : members)
        {
            Luau::visit(
                overloaded{
                    [&](const AstClassProperty& prop)
                    {
                        if (prop.ty)
                            prop.ty->visit(visitor);
                    },
                    [&](const AstClassMethod& method)
                    {
                        method.function->visit(visitor);
                    }
                },
                member
            );
        }
    }
}

AstStatDeclareFunction::AstStatDeclareFunction(
    const Location& location,
    const AstArray<AstAttr*>& attributes,
    const AstName& name,
    const Location& nameLocation,
    const AstArray<AstGenericType*>& generics,
    const AstArray<AstGenericTypePack*>& genericPacks,
    const AstTypeList& params,
    const AstArray<AstArgumentName>& paramNames,
    bool vararg,
    const Location& varargLocation,
    AstTypePack* retTypes
)
    : AstStat(ClassIndex(), location)
    , attributes(attributes)
    , name(name)
    , nameLocation(nameLocation)
    , generics(generics)
    , genericPacks(genericPacks)
    , params(params)
    , paramNames(paramNames)
    , vararg(vararg)
    , varargLocation(varargLocation)
    , retTypes(retTypes)
{
}

void AstStatDeclareFunction::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        visitTypeList(visitor, params);
        retTypes->visit(visitor);
    }
}

bool AstStatDeclareFunction::isCheckedFunction() const
{
    for (const AstAttr* attr : attributes)
    {
        if (attr->type == AstAttr::Type::Checked)
            return true;
    }

    return false;
}

bool AstStatDeclareFunction::hasAttribute(AstAttr::Type attributeType) const
{
    return hasAttributeInArray(attributes, attributeType);
}

AstAttr* AstStatDeclareFunction::getAttribute(const AstAttr::Type attributeType) const
{
    return findAttributeInArray(attributes, attributeType);
}

AstStatDeclareExternType::AstStatDeclareExternType(
    const Location& location,
    const AstName& name,
    std::optional<AstName> superName,
    const AstArray<AstDeclaredExternTypeProperty>& props,
    AstTableIndexer* indexer
)
    : AstStat(ClassIndex(), location)
    , name(name)
    , superName(superName)
    , props(props)
    , indexer(indexer)
{
}

void AstStatDeclareExternType::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        for (const AstDeclaredExternTypeProperty& prop : props)
            prop.ty->visit(visitor);
    }
}

AstStatError::AstStatError(
    const Location& location,
    const AstArray<AstExpr*>& expressions,
    const AstArray<AstStat*>& statements,
    unsigned messageIndex
)
    : AstStat(ClassIndex(), location)
    , expressions(expressions)
    , statements(statements)
    , messageIndex(messageIndex)
{
}

void AstStatError::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        for (AstNode* expression : expressions)
            expression->visit(visitor);

        for (AstNode* statement : statements)
            statement->visit(visitor);
    }
}

AstTypeReference::AstTypeReference(
    const Location& location,
    std::optional<AstName> prefix,
    AstName name,
    std::optional<Location> prefixLocation,
    const Location& nameLocation,
    bool hasParameterList,
    const AstArray<AstTypeOrPack>& parameters,
    AstLocal* prefixLocal
)
    : AstType(ClassIndex(), location)
    , hasParameterList(hasParameterList)
    , prefix(prefix)
    , prefixLocation(prefixLocation)
    , prefixLocal(prefixLocal)
    , name(name)
    , nameLocation(nameLocation)
    , parameters(parameters)
{
}

void AstTypeReference::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        visitTypeOrPackArray(visitor, parameters);
    }
}

AstTypeTable::AstTypeTable(const Location& location, const AstArray<AstTableProp>& props, AstTableIndexer* indexer, bool isExact)
    : AstType(ClassIndex(), location)
    , props(props)
    , indexer(indexer)
    , isExact(isExact)
{
}

void AstTypeTable::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        for (const AstTableProp& prop : props)
            prop.type->visit(visitor);

        if (indexer)
        {
            indexer->indexType->visit(visitor);
            indexer->resultType->visit(visitor);
        }
    }
}

AstTypeFunction::AstTypeFunction(
    const Location& location,
    const AstArray<AstGenericType*>& generics,
    const AstArray<AstGenericTypePack*>& genericPacks,
    const AstTypeList& argTypes,
    const AstArray<std::optional<AstArgumentName>>& argNames,
    AstTypePack* returnTypes
)
    : AstType(ClassIndex(), location)
    , attributes()
    , generics(generics)
    , genericPacks(genericPacks)
    , argTypes(argTypes)
    , argNames(argNames)
    , returnTypes(returnTypes)
{
    LUAU_ASSERT(argNames.size == 0 || argNames.size == argTypes.types.size);
}

AstTypeFunction::AstTypeFunction(
    const Location& location,
    const AstArray<AstAttr*>& attributes,
    const AstArray<AstGenericType*>& generics,
    const AstArray<AstGenericTypePack*>& genericPacks,
    const AstTypeList& argTypes,
    const AstArray<std::optional<AstArgumentName>>& argNames,
    AstTypePack* returnTypes
)
    : AstType(ClassIndex(), location)
    , attributes(attributes)
    , generics(generics)
    , genericPacks(genericPacks)
    , argTypes(argTypes)
    , argNames(argNames)
    , returnTypes(returnTypes)
{
    LUAU_ASSERT(argNames.size == 0 || argNames.size == argTypes.types.size);
}

void AstTypeFunction::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        visitTypeList(visitor, argTypes);
        returnTypes->visit(visitor);
    }
}

bool AstTypeFunction::isCheckedFunction() const
{
    for (const AstAttr* attr : attributes)
    {
        if (attr->type == AstAttr::Type::Checked)
            return true;
    }

    return false;
}

bool AstTypeFunction::hasAttribute(AstAttr::Type attributeType) const
{
    return hasAttributeInArray(attributes, attributeType);
}

AstAttr* AstTypeFunction::getAttribute(AstAttr::Type attributeType) const
{
    return findAttributeInArray(attributes, attributeType);
}

AstTypeTypeof::AstTypeTypeof(const Location& location, AstExpr* expr)
    : AstType(ClassIndex(), location)
    , expr(expr)
{
}

void AstTypeTypeof::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
        expr->visit(visitor);
}

AstTypeOptional::AstTypeOptional(const Location& location)
    : AstType(ClassIndex(), location)
{
}

void AstTypeOptional::visit(AstVisitor* visitor)
{
    visitor->visit(this);
}

AstTypeUnion::AstTypeUnion(const Location& location, const AstArray<AstType*>& types)
    : AstType(ClassIndex(), location)
    , types(types)
{
}

void AstTypeUnion::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        for (AstType* type : types)
            type->visit(visitor);
    }
}

AstTypeIntersection::AstTypeIntersection(const Location& location, const AstArray<AstType*>& types)
    : AstType(ClassIndex(), location)
    , types(types)
{
}

void AstTypeIntersection::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        for (AstType* type : types)
            type->visit(visitor);
    }
}

AstTypeSingletonBool::AstTypeSingletonBool(const Location& location, bool value)
    : AstType(ClassIndex(), location)
    , value(value)
{
}

void AstTypeSingletonBool::visit(AstVisitor* visitor)
{
    visitor->visit(this);
}

AstTypeSingletonString::AstTypeSingletonString(const Location& location, const AstArray<char>& value)
    : AstType(ClassIndex(), location)
    , value(value)
{
}

void AstTypeSingletonString::visit(AstVisitor* visitor)
{
    visitor->visit(this);
}

AstTypeGroup::AstTypeGroup(const Location& location, AstType* type)
    : AstType(ClassIndex(), location)
    , type(type)
{
}

void AstTypeGroup::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
        type->visit(visitor);
}

AstTypeError::AstTypeError(const Location& location, const AstArray<AstType*>& types, bool isMissing, unsigned messageIndex)
    : AstType(ClassIndex(), location)
    , types(types)
    , isMissing(isMissing)
    , messageIndex(messageIndex)
{
}

void AstTypeError::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        for (AstType* type : types)
            type->visit(visitor);
    }
}

AstTypePackExplicit::AstTypePackExplicit(const Location& location, AstTypeList typeList)
    : AstTypePack(ClassIndex(), location)
    , typeList(typeList)
{
}

void AstTypePackExplicit::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
    {
        for (AstType* type : typeList.types)
            type->visit(visitor);

        if (typeList.tailType)
            typeList.tailType->visit(visitor);
    }
}

AstTypePackVariadic::AstTypePackVariadic(const Location& location, AstType* variadicType)
    : AstTypePack(ClassIndex(), location)
    , variadicType(variadicType)
{
}

void AstTypePackVariadic::visit(AstVisitor* visitor)
{
    if (visitor->visit(this))
        variadicType->visit(visitor);
}

AstTypePackGeneric::AstTypePackGeneric(const Location& location, AstName name)
    : AstTypePack(ClassIndex(), location)
    , genericName(name)
{
}

void AstTypePackGeneric::visit(AstVisitor* visitor)
{
    visitor->visit(this);
}

bool isLValue(const AstExpr* expr)
{
    return expr->is<AstExprLocal>() || expr->is<AstExprGlobal>() || expr->is<AstExprIndexName>() || expr->is<AstExprIndexExpr>();
}

bool isConstantLiteral(const AstExpr* expr)
{
    return expr->is<AstExprConstantNil>() || expr->is<AstExprConstantBool>() || expr->is<AstExprConstantNumber>() ||
           expr->is<AstExprConstantString>();
}

bool isLiteralTable(const AstExpr* expr)
{
    if (!expr->is<AstExprTable>())
        return false;

    for (const AstExprTable::Item& item : expr->as<AstExprTable>()->items)
    {
        switch (item.kind)
        {
        case AstExprTable::Item::Kind::General:
            return false;
            break;
        case AstExprTable::Item::Kind::Record:
        case AstExprTable::Item::Kind::List:
            if (!isConstantLiteral(item.value) && !isLiteralTable(item.value))
                return false;
            break;
        }
    }
    return true;
}

AstName getIdentifier(AstExpr* node)
{
    if (AstExprGlobal* expr = node->as<AstExprGlobal>())
        return expr->name;

    if (AstExprLocal* expr = node->as<AstExprLocal>())
        return expr->local->name;

    return AstName();
}

Location getLocation(const AstTypeList& typeList)
{
    Location result;
    if (typeList.types.size)
    {
        result = Location{typeList.types.data[0]->location, typeList.types.data[typeList.types.size - 1]->location};
    }
    if (typeList.tailType)
        result.end = typeList.tailType->location.end;

    return result;
}

} // namespace Luau
