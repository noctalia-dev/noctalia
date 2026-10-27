// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/Ast.h"
#include "Luau/Cst.h"
#include "Luau/Common.h"

namespace Luau
{

int gCstRttiIndex = 0;

CstAttr::CstAttr(bool hasAt)
    : CstNode(CstClassIndex())
    , hasAt(hasAt)
{
}

CstParametrizedAttr::CstParametrizedAttr(Position openParenPosition, Position closeParenPosition, AstArray<Position> argsCommaPositions)
    : CstNode(CstClassIndex())
    , openParenPosition(openParenPosition)
    , closeParenPosition(closeParenPosition)
    , argsCommaPositions(argsCommaPositions)
{
}

CstAttrList::CstAttrList(Position atBracketPosition, Position closeBracketPosition, AstArray<Position> commaPositions)
    : atBracketPosition(atBracketPosition)
    , closeBracketPosition(closeBracketPosition)
    , commaPositions(commaPositions)
{
}

CstExprGroup::CstExprGroup(Position closePosition)
    : CstNode(CstClassIndex())
    , closePosition(closePosition)
{
}

CstExprConstantNumber::CstExprConstantNumber(const AstArray<char>& value)
    : CstNode(CstClassIndex())
    , value(value)
{
}

CstExprConstantString::CstExprConstantString(AstArray<char> sourceString, QuoteStyle quoteStyle, unsigned int blockDepth)
    : CstNode(CstClassIndex())
    , sourceString(sourceString)
    , quoteStyle(quoteStyle)
    , blockDepth(blockDepth)
{
    LUAU_ASSERT(blockDepth == 0 || quoteStyle == QuoteStyle::QuotedRaw);
}

CstExprConstantInteger::CstExprConstantInteger(const AstArray<char>& value)
    : CstNode(CstClassIndex())
    , value(value)
{
}

CstExprCall::CstExprCall(Position openParens, Position closeParens, AstArray<Position> commaPositions)
    : CstNode(CstClassIndex())
    , openParens(openParens)
    , closeParens(closeParens)
    , commaPositions(commaPositions)
{
}

CstExprIndexExpr::CstExprIndexExpr(Position openBracketPosition, Position closeBracketPosition)
    : CstNode(CstClassIndex())
    , openBracketPosition(openBracketPosition)
    , closeBracketPosition(closeBracketPosition)
{
}

CstExprFunction::CstExprFunction()
    : CstNode(CstClassIndex())
{
}

CstExprTable::CstExprTable(const AstArray<Item>& items)
    : CstNode(CstClassIndex())
    , items(items)
{
}

CstExprOp::CstExprOp(Position opPosition)
    : CstNode(CstClassIndex())
    , opPosition(opPosition)
{
}

CstExprTypeAssertion::CstExprTypeAssertion(Position opPosition)
    : CstNode(CstClassIndex())
    , opPosition(opPosition)
{
}

CstExprIfElse::CstExprIfElse(Position thenPosition, Position elsePosition, bool isElseIf, Position annotationColonPosition)
    : CstNode(CstClassIndex())
    , thenPosition(thenPosition)
    , elsePosition(elsePosition)
    , isElseIf(isElseIf)
    , annotationColonPosition(annotationColonPosition)
{
}

CstExprInterpString::CstExprInterpString(AstArray<AstArray<char>> sourceStrings, AstArray<Position> stringPositions)
    : CstNode(CstClassIndex())
    , sourceStrings(sourceStrings)
    , stringPositions(stringPositions)
{
}

CstExprExplicitTypeInstantiation::CstExprExplicitTypeInstantiation(CstTypeInstantiation instantiation)
    : CstNode(CstClassIndex())
    , instantiation(instantiation)
{
}

CstStatDo::CstStatDo(Position statsStartPosition, Position endPosition)
    : CstNode(CstClassIndex())
    , statsStartPosition(statsStartPosition)
    , endPosition(endPosition)
{
}

CstStatRepeat::CstStatRepeat(Position untilPosition)
    : CstNode(CstClassIndex())
    , untilPosition(untilPosition)
{
}

CstStatReturn::CstStatReturn(AstArray<Position> commaPositions)
    : CstNode(CstClassIndex())
    , commaPositions(commaPositions)
{
}

CstStatLocal::CstStatLocal(
    AstArray<Position> varsAnnotationColonPositions,
    AstArray<Position> varsCommaPositions,
    AstArray<Position> valuesCommaPositions
)
    : CstNode(CstClassIndex())
    , varsAnnotationColonPositions(varsAnnotationColonPositions)
    , varsCommaPositions(varsCommaPositions)
    , valuesCommaPositions(valuesCommaPositions)
{
}

CstStatFor::CstStatFor(Position annotationColonPosition, Position equalsPosition, Position endCommaPosition, Position stepCommaPosition)
    : CstNode(CstClassIndex())
    , annotationColonPosition(annotationColonPosition)
    , equalsPosition(equalsPosition)
    , endCommaPosition(endCommaPosition)
    , stepCommaPosition(stepCommaPosition)
{
}

CstStatForIn::CstStatForIn(
    AstArray<Position> varsAnnotationColonPositions,
    AstArray<Position> varsCommaPositions,
    AstArray<Position> valuesCommaPositions
)
    : CstNode(CstClassIndex())
    , varsAnnotationColonPositions(varsAnnotationColonPositions)
    , varsCommaPositions(varsCommaPositions)
    , valuesCommaPositions(valuesCommaPositions)
{
}

CstStatIf::CstStatIf(Position annotationColonPosition)
    : CstNode(CstClassIndex())
    , annotationColonPosition(annotationColonPosition)
{
}

CstStatAssign::CstStatAssign(AstArray<Position> varsCommaPositions, Position equalsPosition, AstArray<Position> valuesCommaPositions)
    : CstNode(CstClassIndex())
    , varsCommaPositions(varsCommaPositions)
    , equalsPosition(equalsPosition)
    , valuesCommaPositions(valuesCommaPositions)
{
}

CstStatCompoundAssign::CstStatCompoundAssign(Position opPosition)
    : CstNode(CstClassIndex())
    , opPosition(opPosition)
{
}

CstStatFunction::CstStatFunction(Position functionKeywordPosition)
    : CstNode(CstClassIndex())
    , attrLists({})
    , functionKeywordPosition(functionKeywordPosition)
{
}

CstStatFunction::CstStatFunction(AstArray<CstAttrList*> attrLists, Position functionKeywordPosition)
    : CstNode(CstClassIndex())
    , attrLists(attrLists)
    , functionKeywordPosition(functionKeywordPosition)
{
}

CstStatLocalFunction::CstStatLocalFunction(Position localKeywordPosition, Position functionKeywordPosition)
    : CstNode(CstClassIndex())
    , attrLists({})
    , localKeywordPosition(localKeywordPosition)
    , functionKeywordPosition(functionKeywordPosition)
{
}

CstStatLocalFunction::CstStatLocalFunction(AstArray<CstAttrList*> attrLists, Position localKeywordPosition, Position functionKeywordPosition)
    : CstNode(CstClassIndex())
    , attrLists(attrLists)
    , localKeywordPosition(localKeywordPosition)
    , functionKeywordPosition(functionKeywordPosition)
{
}

CstGenericType::CstGenericType(Position defaultEqualsPosition)
    : CstNode(CstClassIndex())
    , defaultEqualsPosition(defaultEqualsPosition)
{
}

CstGenericTypePack::CstGenericTypePack(Position ellipsisPosition, Position defaultEqualsPosition)
    : CstNode(CstClassIndex())
    , ellipsisPosition(ellipsisPosition)
    , defaultEqualsPosition(defaultEqualsPosition)
{
}

CstStatTypeAlias::CstStatTypeAlias(
    Position typeKeywordPosition,
    Position genericsOpenPosition,
    AstArray<Position> genericsCommaPositions,
    Position genericsClosePosition,
    Position equalsPosition
)
    : CstNode(CstClassIndex())
    , typeKeywordPosition(typeKeywordPosition)
    , genericsOpenPosition(genericsOpenPosition)
    , genericsCommaPositions(genericsCommaPositions)
    , genericsClosePosition(genericsClosePosition)
    , equalsPosition(equalsPosition)
{
}

CstStatTypeFunction::CstStatTypeFunction(Position typeKeywordPosition, Position functionKeywordPosition)
    : CstNode(CstClassIndex())
    , typeKeywordPosition(typeKeywordPosition)
    , functionKeywordPosition(functionKeywordPosition)
{
}

CstTypeReference::CstTypeReference(
    Position prefixPointPosition,
    Position openParametersPosition,
    AstArray<Position> parametersCommaPositions,
    Position closeParametersPosition
)
    : CstNode(CstClassIndex())
    , prefixPointPosition(prefixPointPosition)
    , openParametersPosition(openParametersPosition)
    , parametersCommaPositions(parametersCommaPositions)
    , closeParametersPosition(closeParametersPosition)
{
}

CstTypeTable::CstTypeTable(AstArray<Item> items, bool isArray)
    : CstNode(CstClassIndex())
    , items(items)
    , isArray(isArray)
{
}

CstTypeFunction::CstTypeFunction(
    Position openGenericsPosition,
    AstArray<Position> genericsCommaPositions,
    Position closeGenericsPosition,
    Position openArgsPosition,
    AstArray<Position> argumentNameColonPositions,
    AstArray<Position> argumentsCommaPositions,
    Position closeArgsPosition,
    Position returnArrowPosition
)
    : CstNode(CstClassIndex())
    , openGenericsPosition(openGenericsPosition)
    , genericsCommaPositions(genericsCommaPositions)
    , closeGenericsPosition(closeGenericsPosition)
    , openArgsPosition(openArgsPosition)
    , argumentNameColonPositions(argumentNameColonPositions)
    , argumentsCommaPositions(argumentsCommaPositions)
    , closeArgsPosition(closeArgsPosition)
    , returnArrowPosition(returnArrowPosition)
{
}

CstTypeTypeof::CstTypeTypeof(Position openPosition, Position closePosition)
    : CstNode(CstClassIndex())
    , openPosition(openPosition)
    , closePosition(closePosition)
{
}

CstTypeUnion::CstTypeUnion(Position leadingPosition, AstArray<Position> separatorPositions)
    : CstNode(CstClassIndex())
    , leadingPosition(leadingPosition)
    , separatorPositions(separatorPositions)
{
}

CstTypeIntersection::CstTypeIntersection(Position leadingPosition, AstArray<Position> separatorPositions)
    : CstNode(CstClassIndex())
    , leadingPosition(leadingPosition)
    , separatorPositions(separatorPositions)
{
}

CstTypeSingletonString::CstTypeSingletonString(AstArray<char> sourceString, CstExprConstantString::QuoteStyle quoteStyle, unsigned int blockDepth)
    : CstNode(CstClassIndex())
    , sourceString(sourceString)
    , quoteStyle(quoteStyle)
    , blockDepth(blockDepth)
{
    LUAU_ASSERT(quoteStyle != CstExprConstantString::QuoteStyle::QuotedInterp);
}

CstTypeGroup::CstTypeGroup(Position closePosition)
    : CstNode(CstClassIndex())
    , closePosition(closePosition)
{
}

CstTypePackExplicit::CstTypePackExplicit()
    : CstNode(CstClassIndex())
    , openParenthesesPosition(Position::missing())
    , closeParenthesesPosition(Position::missing())
    , commaPositions({})
{
}

CstTypePackExplicit::CstTypePackExplicit(Position openParenthesesPosition, Position closeParenthesesPosition, AstArray<Position> commaPositions)
    : CstNode(CstClassIndex())
    , openParenthesesPosition(openParenthesesPosition)
    , closeParenthesesPosition(closeParenthesesPosition)
    , commaPositions(commaPositions)
{
}

CstTypePackGeneric::CstTypePackGeneric(Position ellipsisPosition)
    : CstNode(CstClassIndex())
    , ellipsisPosition(ellipsisPosition)
{
}

} // namespace Luau
