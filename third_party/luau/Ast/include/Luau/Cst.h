// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/Ast.h"
#include "Luau/Location.h"

namespace Luau
{

extern int gCstRttiIndex;

template<typename T>
struct CstRtti
{
    static const int value;
};

template<typename T>
const int CstRtti<T>::value = ++gCstRttiIndex;

#define LUAU_CST_RTTI(Class) \
    static int CstClassIndex() \
    { \
        return CstRtti<Class>::value; \
    }

class CstNode
{
public:
    explicit CstNode(int classIndex)
        : classIndex(classIndex)
    {
    }

    template<typename T>
    bool is() const
    {
        return classIndex == T::CstClassIndex();
    }
    template<typename T>
    T* as()
    {
        return classIndex == T::CstClassIndex() ? static_cast<T*>(this) : nullptr;
    }
    template<typename T>
    const T* as() const
    {
        return classIndex == T::CstClassIndex() ? static_cast<const T*>(this) : nullptr;
    }

    const int classIndex;
};

class CstAttr : public CstNode
{
public:
    LUAU_CST_RTTI(CstAttr)

    explicit CstAttr(bool hasAt);

    bool hasAt; // false when inside an attribute list, ie @[native checked]
};

class CstParametrizedAttr : public CstNode
{
public:
    LUAU_CST_RTTI(CstParametrizedAttr)

    explicit CstParametrizedAttr(Position openParenPosition, Position closeParenPosition, AstArray<Position> argsCommaPositions);

    Position openParenPosition; // for `@x(args)` form
    Position closeParenPosition;

    // Commas inside the `(a, b, c)` arg list
    AstArray<Position> argsCommaPositions;
};

struct CstAttrList
{
    explicit CstAttrList(Position atBracketPosition, Position closeBracketPosition, AstArray<Position> commaPositions);

    Position atBracketPosition;
    Position closeBracketPosition;
    AstArray<Position> commaPositions;
};

class CstExprGroup : public CstNode
{
public:
    LUAU_CST_RTTI(CstExprGroup)

    explicit CstExprGroup(Position closePosition);

    Position closePosition;
};

class CstExprConstantNumber : public CstNode
{
public:
    LUAU_CST_RTTI(CstExprConstantNumber)

    explicit CstExprConstantNumber(const AstArray<char>& value);

    AstArray<char> value;
};

class CstExprConstantInteger : public CstNode
{
public:
    LUAU_CST_RTTI(CstExprConstantInteger)

    explicit CstExprConstantInteger(const AstArray<char>& value);

    AstArray<char> value;
};

class CstExprConstantString : public CstNode
{
public:
    LUAU_CST_RTTI(CstExprConstantString)

    enum class QuoteStyle
    {
        QuotedSingle,
        QuotedDouble,
        QuotedRaw,
        QuotedInterp,
    };

    CstExprConstantString(AstArray<char> sourceString, QuoteStyle quoteStyle, unsigned int blockDepth);

    AstArray<char> sourceString;
    QuoteStyle quoteStyle;
    unsigned int blockDepth;
};

// Shared between the expression and call nodes
struct CstTypeInstantiation
{
    Position leftArrow1Position = Position::missing();
    Position leftArrow2Position = Position::missing();

    AstArray<Position> commaPositions = {};

    Position rightArrow1Position = Position::missing();
    Position rightArrow2Position = Position::missing();
};

class CstExprCall : public CstNode
{
public:
    LUAU_CST_RTTI(CstExprCall)

    CstExprCall(Position openParens, Position closeParens, AstArray<Position> commaPositions);

    Position openParens;
    Position closeParens;
    AstArray<Position> commaPositions;
    CstTypeInstantiation* explicitTypes = nullptr;
};

class CstExprIndexExpr : public CstNode
{
public:
    LUAU_CST_RTTI(CstExprIndexExpr)

    CstExprIndexExpr(Position openBracketPosition, Position closeBracketPosition);

    Position openBracketPosition;
    Position closeBracketPosition;
};

class CstExprFunction : public CstNode
{
public:
    LUAU_CST_RTTI(CstExprFunction)

    CstExprFunction();

    AstArray<CstAttrList*> attrLists = {};
    Position functionKeywordPosition = Position::missing();
    Position openGenericsPosition = Position::missing();
    AstArray<Position> genericsCommaPositions;
    Position closeGenericsPosition = Position::missing();
    AstArray<Position> argsAnnotationColonPositions;
    AstArray<Position> argsCommaPositions;
    Position varargAnnotationColonPosition = Position::missing();
    Position returnSpecifierPosition = Position::missing();
};

class CstExprTable : public CstNode
{
public:
    LUAU_CST_RTTI(CstExprTable)

    enum class Separator
    {
        Comma,
        Semicolon,
        Missing
    };

    struct Item
    {
        Position indexerOpenPosition;  // '[', only if Kind == General
        Position indexerClosePosition; // ']', only if Kind == General
        Position equalsPosition;       // only if Kind != List
        Separator separator;           // may be missing for last Item
        Position separatorPosition;
    };

    explicit CstExprTable(const AstArray<Item>& items);

    AstArray<Item> items;
};

// TODO: Shared between unary and binary, should we split?
class CstExprOp : public CstNode
{
public:
    LUAU_CST_RTTI(CstExprOp)

    explicit CstExprOp(Position opPosition);

    Position opPosition;
};

class CstExprTypeAssertion : public CstNode
{
public:
    LUAU_CST_RTTI(CstExprTypeAssertion)

    explicit CstExprTypeAssertion(Position opPosition);

    Position opPosition;
};

class CstExprIfElse : public CstNode
{
public:
    LUAU_CST_RTTI(CstExprIfElse)

    CstExprIfElse(Position thenPosition, Position elsePosition, bool isElseIf, Position annotationColonPosition = Position::missing());

    Position thenPosition;
    Position elsePosition;
    bool isElseIf;

    // Colon before the `if local`/`if const` binding's type annotation
    Position annotationColonPosition;
};

class CstExprInterpString : public CstNode
{
public:
    LUAU_CST_RTTI(CstExprInterpString)

    explicit CstExprInterpString(AstArray<AstArray<char>> sourceStrings, AstArray<Position> stringPositions);

    AstArray<AstArray<char>> sourceStrings;
    AstArray<Position> stringPositions;
};

class CstExprExplicitTypeInstantiation : public CstNode
{
public:
    LUAU_CST_RTTI(CstExprExplicitTypeInstantiation)

    explicit CstExprExplicitTypeInstantiation(CstTypeInstantiation instantiation);

    CstTypeInstantiation instantiation;
};

class CstStatDo : public CstNode
{
public:
    LUAU_CST_RTTI(CstStatDo)

    explicit CstStatDo(Position statsStartPosition, Position endPosition);

    Position statsStartPosition;
    Position endPosition;
};

class CstStatRepeat : public CstNode
{
public:
    LUAU_CST_RTTI(CstStatRepeat)

    explicit CstStatRepeat(Position untilPosition);

    Position untilPosition;
};

class CstStatReturn : public CstNode
{
public:
    LUAU_CST_RTTI(CstStatReturn)

    explicit CstStatReturn(AstArray<Position> commaPositions);

    AstArray<Position> commaPositions;
};

class CstStatLocal : public CstNode
{
public:
    LUAU_CST_RTTI(CstStatLocal)

    CstStatLocal(AstArray<Position> varsAnnotationColonPositions, AstArray<Position> varsCommaPositions, AstArray<Position> valuesCommaPositions);

    AstArray<Position> varsAnnotationColonPositions;
    AstArray<Position> varsCommaPositions;
    AstArray<Position> valuesCommaPositions;
};

class CstStatFor : public CstNode
{
public:
    LUAU_CST_RTTI(CstStatFor)

    CstStatFor(Position annotationColonPosition, Position equalsPosition, Position endCommaPosition, Position stepCommaPosition);

    Position annotationColonPosition;
    Position equalsPosition;
    Position endCommaPosition;
    Position stepCommaPosition;
};

class CstStatForIn : public CstNode
{
public:
    LUAU_CST_RTTI(CstStatForIn)

    CstStatForIn(AstArray<Position> varsAnnotationColonPositions, AstArray<Position> varsCommaPositions, AstArray<Position> valuesCommaPositions);

    AstArray<Position> varsAnnotationColonPositions;
    AstArray<Position> varsCommaPositions;
    AstArray<Position> valuesCommaPositions;
};

class CstStatIf : public CstNode
{
public:
    LUAU_CST_RTTI(CstStatIf)

    explicit CstStatIf(Position annotationColonPosition);

    // Colon before the `if local`/`if const` binding's type annotation
    Position annotationColonPosition;
};

class CstStatAssign : public CstNode
{
public:
    LUAU_CST_RTTI(CstStatAssign)

    CstStatAssign(AstArray<Position> varsCommaPositions, Position equalsPosition, AstArray<Position> valuesCommaPositions);

    AstArray<Position> varsCommaPositions;
    Position equalsPosition;
    AstArray<Position> valuesCommaPositions;
};

class CstStatCompoundAssign : public CstNode
{
public:
    LUAU_CST_RTTI(CstStatCompoundAssign)

    explicit CstStatCompoundAssign(Position opPosition);

    Position opPosition;
};

class CstStatFunction : public CstNode
{
public:
    LUAU_CST_RTTI(CstStatFunction)

    explicit CstStatFunction(Position functionKeywordPosition);
    explicit CstStatFunction(AstArray<CstAttrList*> attrLists, Position functionKeywordPosition);

    AstArray<CstAttrList*> attrLists;
    Position functionKeywordPosition;
};

class CstStatLocalFunction : public CstNode
{
public:
    LUAU_CST_RTTI(CstStatLocalFunction)

    explicit CstStatLocalFunction(Position localKeywordPosition, Position functionKeywordPosition);
    explicit CstStatLocalFunction(AstArray<CstAttrList*> attrLists, Position localKeywordPosition, Position functionKeywordPosition);

    AstArray<CstAttrList*> attrLists;
    Position localKeywordPosition;
    Position functionKeywordPosition;
};

class CstGenericType : public CstNode
{
public:
    LUAU_CST_RTTI(CstGenericType)

    CstGenericType(Position defaultEqualsPosition);

    Position defaultEqualsPosition;
};

class CstGenericTypePack : public CstNode
{
public:
    LUAU_CST_RTTI(CstGenericTypePack)

    CstGenericTypePack(Position ellipsisPosition, Position defaultEqualsPosition);

    Position ellipsisPosition;
    Position defaultEqualsPosition;
};

class CstStatTypeAlias : public CstNode
{
public:
    LUAU_CST_RTTI(CstStatTypeAlias)

    CstStatTypeAlias(
        Position typeKeywordPosition,
        Position genericsOpenPosition,
        AstArray<Position> genericsCommaPositions,
        Position genericsClosePosition,
        Position equalsPosition
    );

    Position typeKeywordPosition;
    Position genericsOpenPosition;
    AstArray<Position> genericsCommaPositions;
    Position genericsClosePosition;
    Position equalsPosition;
};

class CstStatTypeFunction : public CstNode
{
public:
    LUAU_CST_RTTI(CstStatTypeFunction)

    CstStatTypeFunction(Position typeKeywordPosition, Position functionKeywordPosition);

    Position typeKeywordPosition;
    Position functionKeywordPosition;
};

class CstTypeReference : public CstNode
{
public:
    LUAU_CST_RTTI(CstTypeReference)

    CstTypeReference(
        Position prefixPointPosition,
        Position openParametersPosition,
        AstArray<Position> parametersCommaPositions,
        Position closeParametersPosition
    );

    Position prefixPointPosition;
    Position openParametersPosition;
    AstArray<Position> parametersCommaPositions;
    Position closeParametersPosition;
};

class CstTypeTable : public CstNode
{
public:
    LUAU_CST_RTTI(CstTypeTable)

    struct Item
    {
        enum class Kind
        {
            Indexer,
            Property,
            StringProperty,
        };

        Kind kind;
        Position indexerOpenPosition;  // '[', only if Kind != Property
        Position indexerClosePosition; // ']' only if Kind != Property
        Position colonPosition;
        CstExprTable::Separator separator; // may be missing for last Item
        Position separatorPosition;

        CstExprConstantString* stringInfo = nullptr; // only if Kind == StringProperty
        Position stringPosition{0, 0};               // only if Kind == StringProperty
    };

    CstTypeTable(AstArray<Item> items, bool isArray);

    AstArray<Item> items;
    bool isArray = false;
};

class CstTypeFunction : public CstNode
{
public:
    LUAU_CST_RTTI(CstTypeFunction)

    CstTypeFunction(
        Position openGenericsPosition,
        AstArray<Position> genericsCommaPositions,
        Position closeGenericsPosition,
        Position openArgsPosition,
        AstArray<Position> argumentNameColonPositions,
        AstArray<Position> argumentsCommaPositions,
        Position closeArgsPosition,
        Position returnArrowPosition
    );

    Position openGenericsPosition;
    AstArray<Position> genericsCommaPositions;
    Position closeGenericsPosition;
    Position openArgsPosition;
    AstArray<Position> argumentNameColonPositions;
    AstArray<Position> argumentsCommaPositions;
    Position closeArgsPosition;
    Position returnArrowPosition;
};

class CstTypeTypeof : public CstNode
{
public:
    LUAU_CST_RTTI(CstTypeTypeof)

    CstTypeTypeof(Position openPosition, Position closePosition);

    Position openPosition;
    Position closePosition;
};

class CstTypeUnion : public CstNode
{
public:
    LUAU_CST_RTTI(CstTypeUnion)

    CstTypeUnion(Position leadingPosition, AstArray<Position> separatorPositions);

    Position leadingPosition;
    AstArray<Position> separatorPositions;
};

class CstTypeIntersection : public CstNode
{
public:
    LUAU_CST_RTTI(CstTypeIntersection)

    explicit CstTypeIntersection(Position leadingPosition, AstArray<Position> separatorPositions);

    Position leadingPosition;
    AstArray<Position> separatorPositions;
};

class CstTypeSingletonString : public CstNode
{
public:
    LUAU_CST_RTTI(CstTypeSingletonString)

    CstTypeSingletonString(AstArray<char> sourceString, CstExprConstantString::QuoteStyle quoteStyle, unsigned int blockDepth);

    AstArray<char> sourceString;
    CstExprConstantString::QuoteStyle quoteStyle;
    unsigned int blockDepth;
};

class CstTypeGroup : public CstNode
{
public:
    LUAU_CST_RTTI(CstTypeGroup)

    CstTypeGroup(Position closePosition);

    Position closePosition;
};

class CstTypePackExplicit : public CstNode
{
public:
    LUAU_CST_RTTI(CstTypePackExplicit)

    explicit CstTypePackExplicit();
    explicit CstTypePackExplicit(Position openParenthesesPosition, Position closeParenthesesPosition, AstArray<Position> commaPositions);

    Position openParenthesesPosition;
    Position closeParenthesesPosition;
    AstArray<Position> commaPositions;
};

class CstTypePackGeneric : public CstNode
{
public:
    LUAU_CST_RTTI(CstTypePackGeneric)

    explicit CstTypePackGeneric(Position ellipsisPosition);

    Position ellipsisPosition;
};

} // namespace Luau
