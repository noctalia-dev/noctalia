// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/Compiler.h"

#include "Luau/Ast.h"
#include "Luau/BytecodeBuilder.h"
#include "Luau/Common.h"
#include "Luau/Parser.h"
#include "Luau/InsertionOrderedMap.h"
#include "Luau/StringUtils.h"
#include "Luau/TimeTrace.h"

#include "Builtins.h"
#include "ConstantFolding.h"
#include "CostModel.h"
#include "TableShape.h"
#include "Types.h"
#include "Utils.h"
#include "ValueTracking.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <limits>

#include <math.h>
#include <stdlib.h>

LUAU_FASTINTVARIABLE(LuauCompileLoopUnrollThreshold, 25)
LUAU_FASTINTVARIABLE(LuauCompileLoopUnrollThresholdMaxBoost, 300)

LUAU_FASTINTVARIABLE(LuauCompileInlineThreshold, 25)
LUAU_FASTINTVARIABLE(LuauCompileInlineThresholdMaxBoost, 300)
LUAU_FASTINTVARIABLE(LuauCompileInlineDepth, 5)

LUAU_FASTFLAG(LuauExportValueSyntax)
LUAU_FASTFLAGVARIABLE(LuauCompileMoveElision)
LUAU_FASTFLAGVARIABLE(LuauCompileMoveElisionFix)
LUAU_FASTFLAGVARIABLE(LuauCompileCleanBlockDeadClose)
LUAU_FASTFLAGVARIABLE(LuauCompileLoopUnrollZero)
LUAU_FASTFLAG(LuauIntegerType2)
LUAU_FASTFLAGVARIABLE(LuauCompileConcatTargetTop)
LUAU_FASTFLAG(LuauCompileRefactorFeedback)
LUAU_FASTFLAG(DebugLuauNoInline)
LUAU_FASTFLAG(LuauEmitCallFeedback)
LUAU_FASTFLAGVARIABLE(LuauOptimizeExportTable)
LUAU_FASTFLAG(LuauCompileFastpcall)
LUAU_FASTFLAGVARIABLE(LuauExportedTypesParticipateInScc)
LUAU_FLAGVERSION(LuauExportedTypesParticipateInScc, 2)
LUAU_FASTFLAG(LuauExperimentalIfLocalSyntax)
LUAU_FASTFLAGVARIABLE(LuauCompileReuseLocalRegs)

namespace Luau
{

using namespace Luau::Compile;

static const uint32_t kDefaultAllocPc = ~0u;

void escapeAndAppend(std::string& buffer, const char* str, size_t len)
{
    if (memchr(str, '%', len))
    {
        for (size_t characterIndex = 0; characterIndex < len; ++characterIndex)
        {
            char character = str[characterIndex];
            buffer.push_back(character);

            if (character == '%')
                buffer.push_back('%');
        }
    }
    else
        buffer.append(str, len);
}

CompileError::CompileError(const Location& location, std::string message)
    : location(location)
    , message(std::move(message))
{
}

CompileError::~CompileError() throw() {}

const char* CompileError::what() const throw()
{
    return message.c_str();
}

const Location& CompileError::getLocation() const
{
    return location;
}

// NOINLINE is used to limit the stack cost of this function due to std::string object / exception plumbing
LUAU_NOINLINE void CompileError::raise(const Location& location, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::string message = vformat(format, args);
    va_end(args);

    throw CompileError(location, message);
}

static BytecodeBuilder::StringRef sref(AstName name)
{
    LUAU_ASSERT(name.value);
    return {name.value, strlen(name.value)};
}

static BytecodeBuilder::StringRef sref(AstArray<char> data)
{
    LUAU_ASSERT(data.data);
    return {data.data, data.size};
}

static BytecodeBuilder::StringRef sref(AstArray<const char> data)
{
    LUAU_ASSERT(data.data);
    return {data.data, data.size};
}

struct Compiler
{
    struct RegScope;

    Compiler(BytecodeBuilder& bytecode, const CompileOptions& options, AstNameTable& names)
        : bytecode(bytecode)
        , options(options)
        , builtinTypes(options.vectorType)
        , names(names)
        , exports(AstLocal(names.getOrAdd("__EXP"), Location(), nullptr, 0, 0, nullptr, true))
    {
        // preallocate some buffers that are very likely to grow anyway; this works around std::vector's inefficient growth policy for small arrays
        localStack.reserve(16);
        upvals.reserve(16);
    }

    void checkConstant(int32_t constant, const Location& location)
    {
        if (constant < 0)
            CompileError::raise(location, "Exceeded constant limit; simplify the code to compile");
    }

    int getLocalReg(AstLocal* local)
    {
        Local* l = locals.find(local);

        return l && l->allocated ? l->reg : -1;
    }

    uint8_t getUpval(AstLocal* local)
    {
        for (size_t uid = 0; uid < upvals.size(); ++uid)
            if (upvals[uid] == local)
                return uint8_t(uid);

        if (upvals.size() >= kMaxUpvalueCount)
            CompileError::raise(
                local->location, "Out of upvalue registers when trying to allocate %s: exceeded limit %d", local->name.value, kMaxUpvalueCount
            );

        // mark local as captured so that closeLocals emits LOP_CLOSEUPVALS accordingly
        Variable* v = variables.find(local);

        if (v && v->written)
        {
            if (FFlag::LuauCompileMoveElision)
            {
                Local& l = locals[local];
                l.captured = true;

                if (l.allocated)
                    regCaptured[l.reg] = true;
            }
            else
            {
                locals[local].captured = true;
            }
        }

        upvals.push_back(local);

        return uint8_t(upvals.size() - 1);
    }

    bool atTopLevel() const
    {
        return currentFunction != nullptr && currentFunction->functionDepth == 0 && blockDepth == 0 && loops.empty();
    }

    void checkExportedLocal(AstLocal* local, const Location& location)
    {
        if (local->isExported)
        {
            if (!atTopLevel())
            {
                // We can catch some non top-level usages in the parser, but for others, like in loops, we also catch them here
                CompileError::raise(location, "'export' may only be applied to top-level statements");
            }
        }
    }

    void ensureExportTable(AstNode* node)
    {
        exports.hasExports = true;

        if (locals.contains(&exports.exportTableLocal))
            return;

        LUAU_ASSERT(atTopLevel());

        uint8_t tableReg = allocReg(node, 1u);
        if (FFlag::LuauOptimizeExportTable && exports.exportedTableCid != -1 && exports.exportedTableCid < 32768)
        {
            bytecode.emitAD(LOP_DUPTABLE, tableReg, static_cast<int16_t>(exports.exportedTableCid));
        }
        else
        {
            bytecode.emitABC(LOP_NEWTABLE, tableReg, encodeHashSize(0), 0);
            bytecode.emitAux(0);
        }

        pushLocal(&exports.exportTableLocal, tableReg, kDefaultAllocPc);
    }

    uint8_t getExportTableReg(AstNode* node)
    {
        if (int reg = getLocalReg(&exports.exportTableLocal); reg >= 0)
            return uint8_t(reg);

        uint8_t upval = getUpval(&exports.exportTableLocal);
        uint8_t reg = allocReg(node, 1u);
        bytecode.emitABC(LOP_GETUPVAL, reg, upval, 0);
        return reg;
    }

    void buildExportTableShape()
    {
        LUAU_ASSERT(FFlag::LuauOptimizeExportTable);

        BytecodeBuilder::TableShape exportedShape;

        if (exports.exportedVariables.size() >= BytecodeBuilder::TableShape::kMaxLength)
            return;

        for (const auto& exportedLocal : exports.exportedVariables)
        {
            Variable* variable = variables.find(exportedLocal);
            // TODO: this may be fine to just be an assert
            if (variable == nullptr)
            {
                CompileError::raise(exportedLocal->location, "Local does not have corresponding variable");
                continue;
            }

            int32_t keyCid = bytecode.addConstantString(sref(exportedLocal->name));
            if (keyCid < 0)
                // typically we throw a compiler error, but optimistically we can just not build a shape preemptively
                return;

            size_t idx = exportedShape.length;
            exportedShape.keys[idx] = keyCid;
            exportedShape.constants[idx] = -1;
            if (variable->constant && !variable->written)
            {
                int32_t valueCid = getConstantIndex(variable->init);
                // If the constant pool is full we can't embed this value in the shape
                // Bail out so exportedTableCid stays -1 and compileStatLocal falls back to SETTABLEKS
                if (valueCid < 0)
                    return;

                exportedShape.constants[idx] = valueCid;
                exportedShape.hasConstants = true;
            }
            exportedShape.length++;
        }

        if (exportedShape.length > 0)
            exports.exportedTableCid = bytecode.addConstantTable(exportedShape);
    }

    bool alwaysTerminates(AstStat* node) const
    {
        return Compile::alwaysTerminates(constants, node);
    }

    void emitLoadK(uint8_t target, int32_t cid)
    {
        LUAU_ASSERT(cid >= 0);

        if (cid < 32768)
        {
            bytecode.emitAD(LOP_LOADK, target, int16_t(cid));
        }
        else
        {
            bytecode.emitAD(LOP_LOADKX, target, 0);
            bytecode.emitAux(cid);
        }
    }

    AstExpr* tryIndexConstantTable(AstExprIndexName* expr)
    {
        // If we are referring to a local
        AstExprLocal* tableLocal = unwrapExprOfType<AstExprLocal>(expr->expr);
        if (!tableLocal)
            return nullptr;

        // And it's not mutable and has an initializer
        Variable* lv = variables.find(tableLocal->local);
        if (!lv || lv->written || !lv->init)
            return nullptr;

        // And the local is a constant table
        TableConstantKind* tableKind = tableConstants.find(tableLocal->local);
        if (!tableKind || *tableKind != ConstantTable)
            return nullptr;

        AstExprTable* table = unwrapExprOfType<AstExprTable>(lv->init);
        if (!table)
            return nullptr;

        // Look into the initializer to find the function value
        AstExpr* match = nullptr;

        for (const AstExprTable::Item& item : table->items)
        {
            if (item.kind == AstExprTable::Item::Kind::Record || item.kind == AstExprTable::Item::Kind::General)
            {
                Constant* keyConstant = constants.find(item.key);

                // Dynamic key can alias any of the constant keys
                if (!keyConstant)
                {
                    match = nullptr;
                }
                else if (keyConstant->type == Constant::Type_String && keyConstant->stringLength != 0)
                {
                    AstName keyName = names.getOrAdd(keyConstant->valueString, keyConstant->stringLength);

                    // No break as last match determines the lookup result if there are duplicates
                    if (keyName == expr->index)
                        match = item.value;
                }
            }
        }

        return match;
    }

    AstExprFunction* getFunctionExpr(AstExpr* node)
    {
        if (AstExprLocal* expr = node->as<AstExprLocal>())
        {
            Variable* lv = variables.find(expr->local);

            if (!lv || lv->written || !lv->init)
                return nullptr;

            return getFunctionExpr(lv->init);
        }
        else if (AstExprIndexName* expr = node->as<AstExprIndexName>())
        {
            if (AstExpr* value = tryIndexConstantTable(expr))
                return getFunctionExpr(value);

            return nullptr;
        }
        else if (AstExprGroup* expr = node->as<AstExprGroup>())
            return getFunctionExpr(expr->expr);
        else if (AstExprTypeAssertion* expr = node->as<AstExprTypeAssertion>())
            return getFunctionExpr(expr->expr);
        else if (AstExprInstantiate* expr = node->as<AstExprInstantiate>())
            return getFunctionExpr(expr->expr);
        else
            return node->as<AstExprFunction>();
    }

    void compileExportTable()
    {
        LUAU_ASSERT(currentFunction);

        AstExprFunction* locNode = currentFunction;
        int8_t tableReg;

        if (!exports.isEmpty())
        {
            // this arises when we have a module that is only exporting classes
            ensureExportTable(currentFunction);

            tableReg = getLocalReg(&exports.exportTableLocal);
            LUAU_ASSERT(tableReg >= 0);

            if (FFlag::DebugLuauUserDefinedClasses)
            {
                for (auto& [classLocal, classReg] : exports.exportedClasses)
                {
                    BytecodeBuilder::StringRef classNameRef = sref(classLocal->name);
                    int32_t classNameCid = bytecode.addConstantString(classNameRef);
                    if (classNameCid < 0)
                        CompileError::raise(locNode->location, "Exceeded constant limit; simplify the code to compile");

                    bytecode.emitABC(LOP_SETTABLEKS, classReg, tableReg, uint8_t(BytecodeBuilder::getStringHash(classNameRef)));
                    bytecode.emitAux(classNameCid);
                }
            }

            if (FFlag::LuauOptimizeExportTable)
            {
                for (auto& funcLocal : exports.exportedFunctions)
                {
                    int32_t cid = bytecode.addConstantString(sref(funcLocal->name));
                    if (cid < 0)
                        CompileError::raise(funcLocal->location, "Exceeded constant limit; simplify the code to compile");

                    uint8_t funcReg = getLocalReg(funcLocal);

                    bytecode.emitABC(LOP_SETTABLEKS, funcReg, tableReg, uint8_t(BytecodeBuilder::getStringHash(sref(funcLocal->name))));
                    bytecode.emitAux(cid);
                }
            }
        }
        else
        {
            // Type-only exports with no return: emit a fresh empty table
            LUAU_ASSERT(FFlag::LuauExportedTypesParticipateInScc);
            LUAU_ASSERT(exports.hasTypeExports);

            tableReg = allocReg(locNode, 1u);
            bytecode.emitABC(LOP_NEWTABLE, tableReg, encodeHashSize(0), 0);
            bytecode.emitAux(0);
        }

        uint8_t freezeReg = allocReg(locNode, 2u);
        AstName freezeName = names.getOrAdd("freeze");
        int32_t freezeCid = bytecode.addConstantString(sref(freezeName));
        if (freezeCid < 0)
            CompileError::raise(locNode->location, "Exceeded constant limit; simplify the code to compile");

        AstName tableName = names.getOrAdd("table");
        int32_t tableCid = bytecode.addConstantString(sref(tableName));
        if (tableCid < 0)
            CompileError::raise(locNode->location, "Exceeded constant limit; simplify the code to compile");

        // Note: GETIMPORT encoding is limited to 10 bits per object id component
        // otherwise we can fallback to getglobal
        bool getGlobalFallback = true;
        if (tableCid < 1024 && freezeCid < 1024)
        {
            uint32_t iid = BytecodeBuilder::getImportId(tableCid, freezeCid);
            int32_t cid = bytecode.addImport(iid);
            if (cid >= 0 && cid < 32768)
            {
                bytecode.emitAD(LOP_GETIMPORT, freezeReg, int16_t(cid));
                bytecode.emitAux(iid);
                getGlobalFallback = false;
            }
        }

        if (getGlobalFallback)
        {
            bytecode.emitABC(LOP_GETGLOBAL, freezeReg, 0, uint8_t(BytecodeBuilder::getStringHash(sref(tableName))));
            bytecode.emitAux(tableCid);

            bytecode.emitABC(LOP_GETTABLEKS, freezeReg, freezeReg, uint8_t(BytecodeBuilder::getStringHash(sref(freezeName))));
            bytecode.emitAux(freezeCid);
        }

        bytecode.emitABC(LOP_MOVE, uint8_t(freezeReg + 1), tableReg, 0);
        bytecode.emitABC(LOP_CALL, freezeReg, 2, 2);

        closeLocals(0);
        bytecode.emitABC(LOP_RETURN, freezeReg, 2, 0);
    }

    uint32_t compileFunction(AstExprFunction* func, uint8_t& protoflags)
    {
        LUAU_TIMETRACE_SCOPE("Compiler::compileFunction", "Compiler");

        if (func->debugname.value)
            LUAU_TIMETRACE_ARGUMENT("name", func->debugname.value);

        LUAU_ASSERT(!functions.contains(func));
        LUAU_ASSERT(regTop == 0 && stackSize == 0 && localStack.empty() && upvals.empty());
        if (FFlag::LuauExportValueSyntax)
            currentFunction = func;

        if (FFlag::LuauExportValueSyntax && FFlag::LuauOptimizeExportTable && atTopLevel())
            buildExportTableShape();

        RegScope rs(this);

        bool self = func->self != 0;
        uint32_t fid = bytecode.beginFunction(uint8_t(self + func->args.size), func->vararg);

        setDebugLine(func);

        if (func->vararg)
            bytecode.emitABC(LOP_PREPVARARGS, uint8_t(self + func->args.size), 0, 0);

        uint8_t args = allocReg(func, self + unsigned(func->args.size));

        if (func->self)
            pushLocal(func->self, args, kDefaultAllocPc);

        for (size_t i = 0; i < func->args.size; ++i)
            pushLocal(func->args.data[i], uint8_t(args + self + i), kDefaultAllocPc);

        argCount = localStack.size();

        AstStatBlock* stat = func->body;

        bool terminatesEarly = false;
        Location terminationLocation;
        currentFunction = func;

        for (size_t i = 0; i < stat->body.size; ++i)
        {
            AstStat* bodyStat = stat->body.data[i];

            if (FFlag::LuauCompileReuseLocalRegs)
                popStatLocals(stat, bodyStat, 0u);

            compileStat(bodyStat);

            if (alwaysTerminates(bodyStat))
            {
                terminatesEarly = true;
                break;
            }
        }

        // valid function bytecode must always end with RETURN
        // we elide this if we're guaranteed to hit a RETURN statement regardless of the control flow
        if (FFlag::LuauExportValueSyntax)
        {
            setDebugLineEnd(stat);
            // in main
            if (!exports.isEmpty() && atTopLevel())
            {
                compileExportTable();
            }
            else if (FFlag::LuauExportedTypesParticipateInScc && exports.hasTypeExports && !terminatesEarly && atTopLevel())
            {
                compileExportTable();
            }
            else
            {
                if (!terminatesEarly)
                {
                    closeLocals(0);

                    bytecode.emitABC(LOP_RETURN, 0, 1, 0);
                }
            }
        }
        else
        {
            if (!terminatesEarly)
            {
                setDebugLineEnd(stat);
                closeLocals(0);

                bytecode.emitABC(LOP_RETURN, 0, 1, 0);
            }
        }

        // constant folding may remove some upvalue refs from bytecode, so this puts them back
        if (options.optimizationLevel >= 1 && options.debugLevel >= 2)
            gatherConstUpvals(func);

        bytecode.setDebugFunctionLineDefined(func->location.begin.line + 1);

        if (options.debugLevel >= 1 && func->debugname.value)
            bytecode.setDebugFunctionName(sref(func->debugname));

        if (options.debugLevel >= 2 && !upvals.empty())
        {
            for (AstLocal* l : upvals)
                bytecode.pushDebugUpval(sref(l->name));
        }

        if (options.typeInfoLevel >= 1)
        {
            for (AstLocal* l : upvals)
            {
                LuauBytecodeType ty = LBC_TYPE_ANY;

                if (LuauBytecodeType* recordedTy = localTypes.find(l))
                    ty = *recordedTy;

                bytecode.pushUpvalTypeInfo(ty);
            }
        }

        if (options.optimizationLevel >= 1)
            bytecode.foldJumps();

        bool hasLongJumpError = false;
        bytecode.expandJumps(hasLongJumpError);

        if (hasLongJumpError)
            CompileError::raise(func->location, "Exceeded jump distance limit; simplify the code to compile");

        popLocals(0);

        if (bytecode.getInstructionCount() > kMaxInstructionCount)
            CompileError::raise(func->location, "Exceeded function instruction limit; split the function into parts to compile");

        // note: we move types out of typeMap which is safe because compileFunction is only called once per function
        if (std::string* funcType = functionTypes.find(func))
            bytecode.setFunctionTypeInfo(std::move(*funcType));

        // top-level code only executes once so it can be marked as cold if it has no loops; code with loops might be profitable to compile natively
        if (func->functionDepth == 0 && !hasLoops)
            protoflags |= LPF_NATIVE_COLD;

        if (func->hasNativeAttribute())
            protoflags |= LPF_NATIVE_FUNCTION;

        if (FFlag::LuauExportValueSyntax && !exports.isEmpty() && func->functionDepth == 0)
            protoflags |= LPF_USES_EXPORT;

        if (FFlag::LuauExportedTypesParticipateInScc && exports.hasTypeExports && !terminatesEarly && func->functionDepth == 0)
            protoflags |= LPF_USES_EXPORT;

        bool isInlinable = !hasMultiRet && !getfenvUsed && !setfenvUsed;
        uint64_t costModel = 0;
        if (FFlag::LuauEmitCallFeedback && isInlinable && upvals.empty())
        {
            protoflags |= LPF_INLINABLE;
            costModel = modelCost(func->body, func->args.data, func->args.size, builtins, constants);
        }

        bytecode.endFunction(uint8_t(stackSize), uint8_t(upvals.size()), protoflags, costModel);

        Function& f = functions[func];
        f.id = fid;
        f.upvals = upvals;

        // record information for inlining
        if (options.optimizationLevel >= 2 && !func->vararg && !func->self && !getfenvUsed && !setfenvUsed)
        {
            if (FFlag::DebugLuauNoInline && func->hasAttribute(AstAttr::Type::DebugNoinline))
            {
                f.canInline = false;
            }
            else
            {
                f.canInline = true;
            }
            f.stackSize = stackSize;
            f.costModel = costModel == 0 ? modelCost(func->body, func->args.data, func->args.size, builtins, constants) : costModel;

            // track functions that only ever return a single value so that we can convert multret calls to fixedret calls
            if (alwaysTerminates(func->body))
            {
                ReturnVisitor returnVisitor(this, func);
                stat->visit(&returnVisitor);
                f.returnsOne = returnVisitor.returnsOne;

                if (FFlag::LuauCompileMoveElision)
                {
                    f.returnKind = returnVisitor.returnKind;
                    f.returnLocal = returnVisitor.returnLocal;

                    if (f.returnLocal)
                    {
                        if (Local* lc = locals.find(f.returnLocal); lc && lc->captured)
                            f.returnLocalCaptured = true;
                    }
                }
            }
        }

        upvals.clear(); // note: instead of std::move above, we copy & clear to preserve capacity for future pushes
        stackSize = 0;

        if (FFlag::LuauCompileReuseLocalRegs)
            freeLocalRegs.clear();

        argCount = 0;

        hasLoops = false;
        hasMultiRet = false;
        currentFunction = nullptr;

        return fid;
    }

    // returns true if node can return multiple values; may conservatively return true even if expr is known to return just a single value
    bool isExprMultRet(AstExpr* node)
    {
        AstExprCall* expr = node->as<AstExprCall>();
        if (!expr)
            return node->is<AstExprVarargs>();

        // conservative version, optimized for compilation throughput
        if (options.optimizationLevel <= 1)
            return true;

        // handles builtin calls that can be constant-folded
        // without this we may omit some optimizations eg compiling fast calls without use of FASTCALL2K
        if (isConstant(expr))
            return false;

        // handles builtin calls that can't be constant-folded but are known to return one value
        // note: optimizationLevel check is technically redundant but it's important that we never optimize based on builtins in O1
        if (options.optimizationLevel >= 2)
        {
            if (int* bfid = builtins.find(expr); bfid && *bfid != LBF_NONE)
                return getBuiltinInfo(*bfid).results != 1;
        }

        // handles local function calls where we know only one argument is returned
        AstExprFunction* func = getFunctionExpr(expr->func);
        Function* fi = func ? functions.find(func) : nullptr;

        if (fi && fi->returnsOne)
            return false;

        // unrecognized call, so we conservatively assume multret
        return true;
    }

    // note: this doesn't just clobber target (assuming it's temp), but also clobbers *all* allocated registers >= target!
    // this is important to be able to support "multret" semantics due to Lua call frame structure
    bool compileExprTempMultRet(AstExpr* node, uint8_t target)
    {
        if (AstExprCall* expr = node->as<AstExprCall>())
        {
            // Optimization: convert multret calls that always return one value to fixedret calls; this facilitates inlining/constant folding
            if (options.optimizationLevel >= 2 && !isExprMultRet(node))
            {
                compileExprTemp(node, target);
                return false;
            }

            // We temporarily swap out regTop to have targetTop work correctly...
            // This is a crude hack but it's necessary for correctness :(
            RegScope rs(this, target);
            compileExprCall(expr, target, /* targetCount= */ 0, /* targetTop= */ true, /* multRet= */ true);
            return true;
        }
        else if (AstExprVarargs* expr = node->as<AstExprVarargs>())
        {
            // We temporarily swap out regTop to have targetTop work correctly...
            // This is a crude hack but it's necessary for correctness :(
            RegScope rs(this, target);
            compileExprVarargs(expr, target, /* targetCount= */ 0, /* multRet= */ true);
            return true;
        }
        else
        {
            compileExprTemp(node, target);
            return false;
        }
    }

    // note: this doesn't just clobber target (assuming it's temp), but also clobbers *all* allocated registers >= target!
    // this is important to be able to emit code that takes fewer registers and runs faster
    void compileExprTempTop(AstExpr* node, uint8_t target)
    {
        // We temporarily swap out regTop to have targetTop work correctly...
        // This is a crude hack but it's necessary for performance :(
        // It makes sure that nested call expressions can use targetTop optimization and don't need to have too many registers
        RegScope rs(this, target + 1);
        compileExprTemp(node, target);
    }

    void compileExprVarargs(AstExprVarargs* expr, uint8_t target, uint8_t targetCount, bool multRet = false)
    {
        LUAU_ASSERT(targetCount < 255);
        LUAU_ASSERT(!multRet || unsigned(target + targetCount) == regTop);

        setDebugLine(expr); // normally compileExpr sets up line info, but compileExprVarargs can be called directly

        bytecode.emitABC(LOP_GETVARARGS, target, multRet ? 0 : uint8_t(targetCount + 1), 0);
    }

    void compileExprSelectVararg(AstExprCall* expr, uint8_t target, uint8_t targetCount, bool targetTop, bool multRet, uint8_t regs)
    {
        LUAU_ASSERT(targetCount == 1);
        LUAU_ASSERT(!expr->self);
        LUAU_ASSERT(expr->args.size == 2 && expr->args.data[1]->is<AstExprVarargs>());

        AstExpr* arg = expr->args.data[0];

        uint8_t argreg;

        if (int reg = getExprLocalReg(arg); reg >= 0)
            argreg = uint8_t(reg);
        else
        {
            argreg = uint8_t(regs + 1);
            compileExprTempTop(arg, argreg);
        }

        size_t fastcallLabel = bytecode.emitLabel();

        bytecode.emitABC(LOP_FASTCALL1, LBF_SELECT_VARARG, argreg, 0);

        // note, these instructions are normally not executed and are used as a fallback for FASTCALL
        // we can't use TempTop variant here because we need to make sure the arguments we already computed aren't overwritten
        compileExprTemp(expr->func, regs);

        if (argreg != regs + 1)
            bytecode.emitABC(LOP_MOVE, uint8_t(regs + 1), argreg, 0);

        bytecode.emitABC(LOP_GETVARARGS, uint8_t(regs + 2), 0, 0);

        size_t callLabel = bytecode.emitLabel();
        if (!bytecode.patchSkipC(fastcallLabel, callLabel))
            CompileError::raise(expr->func->location, "Exceeded jump distance limit; simplify the code to compile");

        // note, this is always multCall (last argument is variadic)
        bytecode.emitABC(LOP_CALL, regs, 0, multRet ? 0 : uint8_t(targetCount + 1));

        // if we didn't output results directly to target, we need to move them
        if (!targetTop)
        {
            for (size_t i = 0; i < targetCount; ++i)
                bytecode.emitABC(LOP_MOVE, uint8_t(target + i), uint8_t(regs + i), 0);
        }
    }

    void compileExprFastcallN(
        AstExprCall* expr,
        uint8_t target,
        uint8_t targetCount,
        bool targetTop,
        bool multRet,
        uint8_t regs,
        int bfid,
        int bfK = -1
    )
    {
        LUAU_ASSERT(!expr->self);
        LUAU_ASSERT(expr->args.size >= 1);
        LUAU_ASSERT(expr->args.size <= 3);
        LUAU_ASSERT(bfid == LBF_BIT32_EXTRACTK ? bfK >= 0 : bfK < 0);
        LUAU_ASSERT(targetCount < 255);

        LuauOpcode opc = LOP_NOP;

        if (expr->args.size == 1)
            opc = LOP_FASTCALL1;
        else if (bfK >= 0 || (expr->args.size == 2 && isConstant(expr->args.data[1])))
            opc = LOP_FASTCALL2K;
        else if (expr->args.size == 2)
            opc = LOP_FASTCALL2;
        else
            opc = LOP_FASTCALL3;

        uint32_t args[3] = {};

        for (size_t i = 0; i < expr->args.size; ++i)
        {
            if (i > 0 && opc == LOP_FASTCALL2K)
            {
                int32_t cid = getConstantIndex(expr->args.data[i]);
                if (cid < 0)
                    CompileError::raise(expr->location, "Exceeded constant limit; simplify the code to compile");

                args[i] = cid;
            }
            else if (int reg = getExprLocalReg(expr->args.data[i]); reg >= 0)
            {
                args[i] = uint8_t(reg);
            }
            else
            {
                args[i] = uint8_t(regs + 1 + i);

                if (FFlag::LuauCompileMoveElision)
                {
                    // We temporarily swap out regTop to have targetTop work correctly here, see compileExprTempTop for info
                    RegScope rs(this, args[i] + 1);
                    args[i] = compileExprAutoTemp(expr->args.data[i], uint8_t(args[i]));
                }
                else
                {
                    compileExprTempTop(expr->args.data[i], uint8_t(args[i]));
                }
            }
        }

        size_t fastcallLabel = bytecode.emitLabel();

        bytecode.emitABC(opc, uint8_t(bfid), uint8_t(args[0]), 0);

        if (opc == LOP_FASTCALL3)
        {
            LUAU_ASSERT(bfK < 0);
            bytecode.emitAux(args[1] | (args[2] << 8));
        }
        else if (opc != LOP_FASTCALL1)
        {
            bytecode.emitAux(bfK >= 0 ? bfK : args[1]);
        }

        // Set up a traditional Lua stack for the subsequent LOP_CALL.
        // Note, as with other instructions that immediately follow FASTCALL, these are normally not executed and are used as a fallback for
        // these FASTCALL variants.
        for (size_t i = 0; i < expr->args.size; ++i)
        {
            if (i > 0 && opc == LOP_FASTCALL2K)
                emitLoadK(uint8_t(regs + 1 + i), args[i]);
            else if (args[i] != regs + 1 + i)
                bytecode.emitABC(LOP_MOVE, uint8_t(regs + 1 + i), uint8_t(args[i]), 0);
        }

        // note, these instructions are normally not executed and are used as a fallback for FASTCALL
        // we can't use TempTop variant here because we need to make sure the arguments we already computed aren't overwritten
        compileExprTemp(expr->func, regs);

        size_t callLabel = bytecode.emitLabel();

        // FASTCALL will skip over the instructions needed to compute function and jump over CALL which must immediately follow the instruction
        // sequence after FASTCALL
        if (!bytecode.patchSkipC(fastcallLabel, callLabel))
            CompileError::raise(expr->func->location, "Exceeded jump distance limit; simplify the code to compile");

        bytecode.emitABC(LOP_CALL, regs, uint8_t(expr->args.size + 1), multRet ? 0 : uint8_t(targetCount + 1));

        // if we didn't output results directly to target, we need to move them
        if (!targetTop)
        {
            for (size_t i = 0; i < targetCount; ++i)
                bytecode.emitABC(LOP_MOVE, uint8_t(target + i), uint8_t(regs + i), 0);
        }
    }

    bool tryCompileInlinedCall(
        AstExprCall* expr,
        AstExprFunction* func,
        uint8_t target,
        uint8_t targetCount,
        bool multRet,
        int thresholdBase,
        int thresholdMaxBoost,
        int depthLimit
    )
    {
        Function* fi = functions.find(func);
        LUAU_ASSERT(fi);

        // make sure we have enough register space
        if (regTop > 128 || fi->stackSize > 32)
        {
            bytecode.addDebugRemark("inlining failed: high register pressure");
            return false;
        }

        // we should ideally aggregate the costs during recursive inlining, but for now simply limit the depth
        if (int(inlineFrames.size()) >= depthLimit)
        {
            bytecode.addDebugRemark("inlining failed: too many inlined frames");
            return false;
        }

        // compiling recursive inlining is difficult because we share constant/variable state but need to bind variables to different registers
        for (InlineFrame& frame : inlineFrames)
            if (frame.func == func)
            {
                bytecode.addDebugRemark("inlining failed: can't inline recursive calls");
                return false;
            }

        // we can't inline multret functions because the caller expects L->top to be adjusted:
        // - inlined return compiles to a JUMP, and we don't have an instruction that adjusts L->top arbitrarily
        // - even if we did, right now all L->top adjustments are immediately consumed by the next instruction, and for now we want to preserve that
        // - additionally, we can't easily compile multret expressions into designated target as computed call arguments will get clobbered
        if (multRet)
        {
            bytecode.addDebugRemark("inlining failed: can't convert fixed returns to multret");
            return false;
        }

        // compute constant bitvector for all arguments to feed the cost model
        bool varc[8] = {};
        bool hasConstant = false;
        for (size_t i = 0; i < func->args.size && i < expr->args.size && i < 8; ++i)
        {
            if (isConstant(expr->args.data[i]))
            {
                varc[i] = true;
                hasConstant = true;
            }
        }

        // if the last argument only returns a single value, all following arguments are nil
        if (expr->args.size != 0 && !isExprMultRet(expr->args.data[expr->args.size - 1]))
        {
            for (size_t i = expr->args.size; i < func->args.size && i < 8; ++i)
            {
                varc[i] = true;
                hasConstant = true;
            }
        }

        // If we had constant arguments that can affect the cost model of this specific call in non-trivial ways
        uint64_t callCostModel = fi->costModel;

        if (hasConstant)
            callCostModel = costModelInlinedCall(expr, func);

        // we use a dynamic cost threshold that's based on the fixed limit boosted by the cost advantage we gain due to inlining
        int inlinedCost = computeCost(callCostModel, varc, std::min(int(func->args.size), 8));
        int baselineCost = computeCost(fi->costModel, nullptr, 0) + 3;
        int inlineProfit = (inlinedCost == 0) ? thresholdMaxBoost : std::min(thresholdMaxBoost, 100 * baselineCost / inlinedCost);

        int threshold = thresholdBase * inlineProfit / 100;
        bool isIife = unwrapExprOfType<AstExprFunction>(expr->func) != nullptr;

        if (inlinedCost > threshold && !isIife)
        {
            bytecode.addDebugRemark("inlining failed: too expensive (cost %d, profit %.2fx)", inlinedCost, double(inlineProfit) / 100);
            return false;
        }

        bytecode.addDebugRemark(
            "inlining succeeded (cost %d, profit %.2fx, depth %d)", inlinedCost, double(inlineProfit) / 100, int(inlineFrames.size())
        );

        compileInlinedCall(expr, func, target, targetCount);
        return true;
    }

    uint64_t costModelInlinedCall(AstExprCall* expr, AstExprFunction* func)
    {
        for (size_t i = 0; i < func->args.size; ++i)
        {
            AstLocal* var = func->args.data[i];
            AstExpr* arg = i < expr->args.size ? expr->args.data[i] : nullptr;

            // last expression is a multret, there are no constant for it and it will fill all values
            if (i + 1 == expr->args.size && func->args.size > expr->args.size && isExprMultRet(arg))
                break;

            // variable gets mutated at some point, so we do not have a constant for it
            if (Variable* vv = variables.find(var); vv && vv->written)
                continue;

            if (arg == nullptr)
                locstants[var] = {Constant::Type_Nil};
            else if (const Constant* cv = constants.find(arg); cv && cv->type != Constant::Type_Unknown)
                locstants[var] = *cv;
        }

        // fold constant values updated above into expressions in the function body, recording changes for undo
        exprChanges.clear();
        localChanges.clear();

        foldConstants(
            constants,
            variables,
            locstants,
            builtinsFold,
            builtinsFoldLibraryK,
            options.vectorPrecision == 1,
            options.libraryMemberConstantCb,
            func->body,
            names,
            tableConstants,
            &exprChanges,
            &localChanges
        );

        // model the cost of the function evaluated with current constants
        uint64_t cost = modelCost(func->body, func->args.data, func->args.size, builtins, constants);

        // clean up constant state for future inlining attempts
        for (size_t i = 0; i < func->args.size; ++i)
        {
            if (Constant* var = locstants.find(func->args.data[i]))
                var->type = Constant::Type_Unknown;
        }

        Compile::undoChanges(constants, exprChanges);
        Compile::undoChanges(locstants, localChanges);

        return cost;
    }

    struct FindRegisterUseByLocal : AstVisitor
    {
        FindRegisterUseByLocal(Compiler* self, uint8_t reg)
            : self(self)
            , reg(reg)
        {
        }

        bool visit(AstExprLocal* node) override
        {
            if (self->getLocalReg(node->local) == reg)
                used = true;

            return !used;
        }

        Compiler* self = nullptr;
        uint8_t reg = 0;
        bool used = false;
    };

    bool isRegisterUsedInCallArguments(AstExprCall* expr, AstExpr* arg, size_t startArg, uint8_t reg)
    {
        FindRegisterUseByLocal visitor(this, reg);

        if (FFlag::LuauCompileMoveElisionFix)
        {
            // Allow direct use of the target register in the current argument
            if (arg && getExprLocalReg(arg) != reg)
                expr->args.data[startArg]->visit(&visitor);

            for (size_t i = startArg + 1; i < expr->args.size && !visitor.used; ++i)
                expr->args.data[i]->visit(&visitor);
        }
        else
        {
            for (size_t i = startArg; i < expr->args.size && !visitor.used; ++i)
                expr->args.data[i]->visit(&visitor);
        }

        return visitor.used;
    }

    void compileInlinedCall(AstExprCall* expr, AstExprFunction* func, uint8_t target, uint8_t targetCount)
    {
        Function* fi = FFlag::LuauCompileMoveElision ? functions.find(func) : nullptr;

        // We try to write result directly to the target register when possible, but there are a few blockers
        // - function captures a reference to the local that is being returned and might close it
        // - there might not be a target register (return value is unused)
        // - target register is captured and might be read before inlined function completes
        bool noTargetMoveElision = FFlag::LuauCompileMoveElision && (fi->returnLocalCaptured || targetCount == 0 || regCaptured[target]);

        RegScope rs(this);

        size_t oldLocals = localStack.size();

        std::vector<InlineArg> args;
        args.reserve(func->args.size);

        // evaluate all arguments; note that we don't emit code for constant arguments (relying on constant folding)
        // note that compiler state (variable registers/values) does not change here - we defer that to a separate loop below to handle nested calls
        for (size_t i = 0; i < func->args.size; ++i)
        {
            AstLocal* var = func->args.data[i];
            AstExpr* arg = i < expr->args.size ? expr->args.data[i] : nullptr;
            bool canUseParameterTarget =
                FFlag::LuauCompileMoveElision && fi->returnKind == ReturnKind::Parameter && fi->returnLocal == var && !noTargetMoveElision;

            if (i + 1 == expr->args.size && func->args.size > expr->args.size && isExprMultRet(arg))
            {
                // if the last argument can return multiple values, we need to compute all of them into the remaining arguments
                unsigned int tail = unsigned(func->args.size - expr->args.size) + 1;
                uint8_t reg = allocReg(arg, tail);
                uint32_t allocpc = bytecode.getDebugPC();

                if (AstExprCall* expr = arg->as<AstExprCall>())
                    compileExprCall(expr, reg, tail, /* targetTop= */ true);
                else if (AstExprVarargs* expr = arg->as<AstExprVarargs>())
                    compileExprVarargs(expr, reg, tail);
                else
                    LUAU_ASSERT(!"Unexpected expression type");

                for (size_t j = i; j < func->args.size; ++j)
                    args.push_back({func->args.data[j], uint8_t(reg + (j - i)), {Constant::Type_Unknown}, allocpc});

                // all remaining function arguments have been allocated and assigned to
                break;
            }
            else if (Variable* vv = variables.find(var); vv && vv->written)
            {
                if (canUseParameterTarget && !isRegisterUsedInCallArguments(expr, arg, FFlag::LuauCompileMoveElisionFix ? i : i + 1, target))
                {
                    if (arg)
                        compileExprTemp(arg, target);
                    else
                        bytecode.emitABC(LOP_LOADNIL, target, 0, 0);

                    args.push_back({var, target, {Constant::Type_Unknown}, kDefaultAllocPc});
                }
                else
                {
                    // if the argument is mutated, we need to allocate a fresh register even if it's a constant
                    uint8_t reg = allocReg(arg, 1u);
                    uint32_t allocpc = bytecode.getDebugPC();

                    if (arg)
                        compileExprTemp(arg, reg);
                    else
                        bytecode.emitABC(LOP_LOADNIL, reg, 0, 0);

                    args.push_back({var, reg, {Constant::Type_Unknown}, allocpc});
                }
            }
            else if (arg == nullptr)
            {
                // since the argument is not mutated, we can simply fold the value into the expressions that need it
                args.push_back({var, kInvalidReg, {Constant::Type_Nil}});
            }
            else if (const Constant* cv = constants.find(arg); cv && cv->type != Constant::Type_Unknown)
            {
                // since the argument is not mutated, we can simply fold the value into the expressions that need it
                args.push_back({var, kInvalidReg, *cv});
            }
            else
            {
                AstExprLocal* le = getExprLocal(arg);
                Variable* lv = le ? variables.find(le->local) : nullptr;

                // if the argument is a local that isn't mutated, we will simply reuse the existing register
                if (int reg = le ? getExprLocalReg(le) : -1; reg >= 0 && (!lv || !lv->written))
                {
                    args.push_back({var, uint8_t(reg), {Constant::Type_Unknown}, kDefaultAllocPc, lv ? lv->init : nullptr});
                }
                else if (canUseParameterTarget && !isRegisterUsedInCallArguments(expr, arg, FFlag::LuauCompileMoveElisionFix ? i : i + 1, target))
                {
                    compileExprTemp(arg, target);

                    args.push_back({var, target, {Constant::Type_Unknown}, kDefaultAllocPc});
                }
                else
                {
                    uint8_t temp = allocReg(arg, 1u);
                    uint32_t allocpc = bytecode.getDebugPC();

                    if (FFlag::LuauCompileMoveElision)
                        temp = compileExprAutoTemp(arg, temp, target);
                    else
                        compileExprTemp(arg, temp);

                    args.push_back({var, temp, {Constant::Type_Unknown}, allocpc, arg});
                }
            }
        }

        // evaluate extra expressions for side effects
        for (size_t i = func->args.size; i < expr->args.size; ++i)
            compileExprSide(expr->args.data[i]);

        // apply all evaluated arguments to the compiler state
        // note: locals use current startpc for debug info, although some of them have been computed earlier; this is similar to compileStatLocal
        for (InlineArg& arg : args)
        {
            if (arg.value.type == Constant::Type_Unknown)
            {
                pushLocal(arg.local, arg.reg, arg.allocpc);

                if (arg.init)
                {
                    if (Variable* lv = variables.find(arg.local))
                        lv->init = arg.init;
                }
            }
            else
            {
                locstants[arg.local] = arg.value;
            }
        }

        AstLocal* resultLocal =
            FFlag::LuauCompileMoveElision && fi->returnKind == ReturnKind::Local && !noTargetMoveElision ? fi->returnLocal : nullptr;

        // the inline frame will be used to compile return statements as well as to reject recursive inlining attempts
        inlineFrames.push_back({func, oldLocals, target, targetCount, resultLocal});

        // this pass tracks which calls are builtins and can be compiled more efficiently
        analyzeBuiltins(inlineBuiltins, globals, variables, options, func->body, names);

        // If we found new builtins, apply them, but record which expressions we changed so we can undo later
        if (!inlineBuiltins.empty())
        {
            for (auto [callExpr, bfid] : inlineBuiltins)
            {
                int& builtin = builtins[callExpr]; // If there was no builtin previously, we will get LBF_NONE

                if (bfid != builtin)
                {
                    inlineBuiltinsBackup[callExpr] = builtin;
                    builtin = bfid;
                }
            }

            inlineBuiltins.clear();
        }

        // fold constant values updated above into expressions in the function body, recording changes for undo
        exprChanges.clear();
        localChanges.clear();

        foldConstants(
            constants,
            variables,
            locstants,
            builtinsFold,
            builtinsFoldLibraryK,
            options.vectorPrecision == 1,
            options.libraryMemberConstantCb,
            func->body,
            names,
            tableConstants,
            &exprChanges,
            &localChanges
        );

        bool terminatesEarly = false;

        for (size_t i = 0; i < func->body->body.size; ++i)
        {
            AstStat* stat = func->body->body.data[i];

            if (FFlag::LuauCompileReuseLocalRegs)
                popStatLocals(func->body, stat, oldLocals);

            compileStat(stat);

            if (alwaysTerminates(stat))
            {
                terminatesEarly = true;

                // Remove the last jump which jumps directly to the next instruction
                InlineFrame& currFrame = inlineFrames.back();
                if (!currFrame.returnJumps.empty() && currFrame.returnJumps.back() == bytecode.emitLabel() - 1)
                {
                    bytecode.undoEmit(LOP_JUMP);
                    currFrame.returnJumps.pop_back();
                }
                break;
            }
        }

        // for the fallthrough path we need to ensure we clear out target registers
        if (!terminatesEarly)
        {
            for (size_t i = 0; i < targetCount; ++i)
                bytecode.emitABC(LOP_LOADNIL, uint8_t(target + i), 0, 0);

            closeLocals(oldLocals);
        }

        popLocals(oldLocals);

        size_t returnLabel = bytecode.emitLabel();
        patchJumps(expr, inlineFrames.back().returnJumps, returnLabel);

        inlineFrames.pop_back();

        // clean up constant state for future inlining attempts
        for (size_t i = 0; i < func->args.size; ++i)
        {
            AstLocal* local = func->args.data[i];

            if (Constant* var = locstants.find(local))
                var->type = Constant::Type_Unknown;

            if (Variable* lv = variables.find(local))
                lv->init = nullptr;
        }

        if (!inlineBuiltinsBackup.empty())
        {
            for (auto [callExpr, bfid] : inlineBuiltinsBackup)
                builtins[callExpr] = bfid;

            inlineBuiltinsBackup.clear();
        }

        Compile::undoChanges(constants, exprChanges);
        Compile::undoChanges(locstants, localChanges);
    }

    void compileExprCall(AstExprCall* expr, uint8_t target, uint8_t targetCount, bool targetTop = false, bool multRet = false)
    {
        LUAU_ASSERT(targetCount < 255);
        LUAU_ASSERT(!targetTop || unsigned(target + targetCount) == regTop);

        setDebugLine(expr); // normally compileExpr sets up line info, but compileExprCall can be called directly

        // try inlining the function
        if (options.optimizationLevel >= 2 && !expr->self)
        {
            AstExprFunction* func = getFunctionExpr(expr->func);
            Function* fi = func ? functions.find(func) : nullptr;

            if (fi && fi->canInline &&
                tryCompileInlinedCall(
                    expr,
                    func,
                    target,
                    targetCount,
                    multRet,
                    FInt::LuauCompileInlineThreshold,
                    FInt::LuauCompileInlineThresholdMaxBoost,
                    FInt::LuauCompileInlineDepth
                ))
                return;

            // add a debug remark for cases when we didn't even call tryCompileInlinedCall
            if (func && !(fi && fi->canInline))
            {
                if (func->vararg)
                    bytecode.addDebugRemark("inlining failed: function is variadic");
                else if (!fi)
                    bytecode.addDebugRemark("inlining failed: can't inline recursive calls");
                else if (getfenvUsed || setfenvUsed)
                    bytecode.addDebugRemark("inlining failed: module uses getfenv/setfenv");
            }
        }

        RegScope rs(this);

        unsigned int regCount = std::max(unsigned(1 + expr->self + expr->args.size), unsigned(targetCount));

        // Optimization: if target points to the top of the stack, we can start the call at oldTop - 1 and won't need MOVE at the end
        uint8_t regs = targetTop ? allocReg(expr, regCount - targetCount) - targetCount : allocReg(expr, regCount);

        uint8_t selfreg = 0;

        int bfid = -1;

        if (options.optimizationLevel >= 1 && !expr->self)
        {
            if (const int* id = builtins.find(expr); id && *id != LBF_NONE)
                bfid = *id;
        }

        if (bfid >= 0 && bytecode.needsDebugRemarks())
        {
            Builtin builtin = getBuiltin(expr->func, globals, variables);
            bool lastMult = expr->args.size > 0 && isExprMultRet(expr->args.data[expr->args.size - 1]);

            if (builtin.object.value)
                bytecode.addDebugRemark("builtin %s.%s/%d%s", builtin.object.value, builtin.method.value, int(expr->args.size), lastMult ? "+" : "");
            else if (builtin.method.value)
                bytecode.addDebugRemark("builtin %s/%d%s", builtin.method.value, int(expr->args.size), lastMult ? "+" : "");
        }

        if (bfid == LBF_SELECT_VARARG)
        {
            // Optimization: compile select(_, ...) as FASTCALL1; the builtin will read variadic arguments directly
            // note: for now we restrict this to single-return expressions since our runtime code doesn't deal with general cases
            if (multRet == false && targetCount == 1)
                return compileExprSelectVararg(expr, target, targetCount, targetTop, multRet, regs);
            else
                bfid = -1;
        }

        // Optimization: for bit32.extract with constant in-range f/w we compile using FASTCALL2K and a special builtin
        if (bfid == LBF_BIT32_EXTRACT && expr->args.size == 3 && isConstant(expr->args.data[1]) && isConstant(expr->args.data[2]))
        {
            Constant fc = getConstant(expr->args.data[1]);
            Constant wc = getConstant(expr->args.data[2]);

            int fi = fc.type == Constant::Type_Number ? int(fc.valueNumber) : -1;
            int wi = wc.type == Constant::Type_Number ? int(wc.valueNumber) : -1;

            if (fi >= 0 && wi > 0 && fi + wi <= 32)
            {
                int fwp = fi | ((wi - 1) << 5);
                int32_t cid = bytecode.addConstantNumber(fwp);
                if (cid < 0)
                    CompileError::raise(expr->location, "Exceeded constant limit; simplify the code to compile");

                return compileExprFastcallN(expr, target, targetCount, targetTop, multRet, regs, LBF_BIT32_EXTRACTK, cid);
            }
        }

        unsigned maxFastcallArgs = 2;

        // Fastcall with 3 arguments is only used if it can help save one or more move instructions
        if (bfid >= 0 && expr->args.size == 3)
        {
            for (size_t i = 0; i < expr->args.size; ++i)
            {
                if (int reg = getExprLocalReg(expr->args.data[i]); reg >= 0)
                {
                    maxFastcallArgs = 3;
                    break;
                }
            }
        }

        // Optimization: for 1/2/3 argument fast calls use specialized opcodes
        if (bfid >= 0 && expr->args.size >= 1 && expr->args.size <= maxFastcallArgs)
        {
            if (!isExprMultRet(expr->args.data[expr->args.size - 1]))
            {
                return compileExprFastcallN(expr, target, targetCount, targetTop, multRet, regs, bfid);
            }
            else if (options.optimizationLevel >= 2)
            {
                // when a builtin is none-safe with matching arity, even if the last expression returns 0 or >1 arguments,
                // we can rely on the behavior of the function being the same (none-safe means nil and none are interchangeable)
                BuiltinInfo info = getBuiltinInfo(bfid);
                if (int(expr->args.size) == info.params && (info.flags & BuiltinInfo::Flag_NoneSafe) != 0)
                    return compileExprFastcallN(expr, target, targetCount, targetTop, multRet, regs, bfid);
            }
        }

        // Optimization: pcall/xpcall functions have a special fastcall instruction
        int fastPcallId = -1;
        if (FFlag::LuauCompileFastpcall && options.optimizationLevel >= 1 && !expr->self)
        {
            if (AstExprGlobal* g = expr->func->as<AstExprGlobal>(); g && canImport(g))
            {
                if (g->name == "pcall" && expr->args.size >= 1)
                    fastPcallId = 0;
                else if (g->name == "xpcall" && expr->args.size >= 2)
                    fastPcallId = 1;
            }
        }

        if (expr->self)
        {
            AstExprIndexName* fi = expr->func->as<AstExprIndexName>();
            LUAU_ASSERT(fi);

            // Optimization: use local register directly in NAMECALL if possible
            if (int reg = getExprLocalReg(fi->expr); reg >= 0)
            {
                selfreg = uint8_t(reg);
            }
            else
            {
                // Note: to be able to compile very deeply nested self call chains (obj:method1():method2():...), we need to be able to do this in
                // finite stack space NAMECALL will happily move object from regs to regs+1 but we need to compute it into regs so that
                // compileExprTempTop doesn't increase stack usage for every recursive call
                selfreg = regs;

                compileExprTempTop(fi->expr, selfreg);
            }
        }
        else if (bfid < 0 && (!FFlag::LuauCompileFastpcall || fastPcallId < 0))
        {
            compileExprTempTop(expr->func, regs);
        }

        bool multCall = false;
        bool constructCall = false;

        if (FFlag::DebugLuauUserDefinedClasses && !expr->self && expr->tableCall && bfid < 0 && fastPcallId < 0)
        {
            LUAU_ASSERT(expr->args.size == 1);
            LUAU_ASSERT(expr->args.data[0]->is<AstExprTable>());

            uint8_t arg = regs + 1;

            RegScope rs(this, arg + 1);
            compileExprTable(expr->args.data[0]->as<AstExprTable>(), arg, true, regs);
            constructCall = true;
        }
        else
        {
            for (size_t i = 0; i < expr->args.size; ++i)
            {
                if (i + 1 == expr->args.size)
                    multCall = compileExprTempMultRet(expr->args.data[i], uint8_t(regs + 1 + expr->self + i));
                else
                    compileExprTempTop(expr->args.data[i], uint8_t(regs + 1 + expr->self + i));
            }
        }

        setDebugLineEnd(expr->func);

        if (expr->self)
        {
            AstExprIndexName* fi = expr->func->as<AstExprIndexName>();
            LUAU_ASSERT(fi);

            setDebugLine(fi->indexLocation);

            BytecodeBuilder::StringRef iname = sref(fi->index);
            int32_t cid = bytecode.addConstantString(iname);
            if (cid < 0)
                CompileError::raise(fi->location, "Exceeded constant limit; simplify the code to compile");

            bytecode.emitABC(LOP_NAMECALL, regs, selfreg, uint8_t(BytecodeBuilder::getStringHash(iname)));
            bytecode.emitAux(cid);

            hintTemporaryExprRegType(fi->expr, selfreg, LBC_TYPE_TABLE, /* instLength */ 2);
        }
        else if (bfid >= 0 || (FFlag::LuauCompileFastpcall && fastPcallId >= 0))
        {
            size_t fastcallLabel = bytecode.emitLabel();

            if (FFlag::LuauCompileFastpcall && fastPcallId >= 0)
            {
                uint8_t explicitArgs = uint8_t(expr->args.size - (multCall ? 1 : 0));
                bytecode.emitABC(LOP_FASTPCALL, uint8_t(fastPcallId), explicitArgs, 0);
            }
            else
            {
                bytecode.emitABC(LOP_FASTCALL, uint8_t(bfid), 0, 0);
            }

            // note, these instructions are normally not executed and are used as a fallback for FASTCALL
            // we can't use TempTop variant here because we need to make sure the arguments we already computed aren't overwritten
            compileExprTemp(expr->func, regs);

            size_t callLabel = bytecode.emitLabel();

            // FASTCALL will skip over the instructions needed to compute function and jump over CALL which must immediately follow the instruction
            // sequence after FASTCALL
            if (!bytecode.patchSkipC(fastcallLabel, callLabel))
                CompileError::raise(expr->func->location, "Exceeded jump distance limit; simplify the code to compile");
        }

        size_t jumpLabel = bytecode.emitLabel();

        if (constructCall)
        {
            LUAU_ASSERT(!expr->self);
            bytecode.emitABC(LOP_FINCONSTRUCT, regs + 1, 0, 0);
        }

        size_t callLabel = bytecode.emitLabel();

        // Without deoptimization we cannot break VARARG sequences.
        // So VARARG producer or consumer cannot be inlined, because it creates a diamond(with slow path).
        bool canInline = currentFunction->functionDepth != 0 && !multCall && !multRet;
        if (FFlag::LuauEmitCallFeedback && (bfid < 0 && (!FFlag::LuauCompileFastpcall || fastPcallId < 0)) && canInline && !constructCall)
        {
            uint32_t fbSlot = FFlag::LuauCompileRefactorFeedback || FFlag::DebugLuauUserDefinedClasses
                                  ? bytecode.addCallTargetSlot(uint32_t(bytecode.getInstructionCount()))
                                  : bytecode.addFbSlot_DEPRECATED(LuauFeedbackType::LFT_CALLTARGET);
            bytecode.emitABC(LOP_CALLFB, regs, multCall ? 0 : uint8_t(expr->self + expr->args.size + 1), multRet ? 0 : uint8_t(targetCount + 1));
            bytecode.emitAux(fbSlot);
        }
        else
        {
            bytecode.emitABC(LOP_CALL, regs, multCall ? 0 : uint8_t(expr->self + expr->args.size + 1), multRet ? 0 : uint8_t(targetCount + 1));
        }

        if (constructCall)
        {
            if (!bytecode.patchSkipC(jumpLabel, callLabel))
                CompileError::raise(expr->location, "Exceeded jump distance limit; simplify the code to compile");
        }

        // if we didn't output results directly to target, we need to move them
        if (!targetTop)
        {
            for (size_t i = 0; i < targetCount; ++i)
                bytecode.emitABC(LOP_MOVE, uint8_t(target + i), uint8_t(regs + i), 0);
        }
    }

    bool shouldShareClosure(AstExprFunction* func)
    {
        const Function* f = functions.find(func);
        if (!f)
            return false;

        for (AstLocal* uv : f->upvals)
        {
            Variable* ul = variables.find(uv);

            if (!ul)
                return false;

            if (ul->written)
                return false;

            // it's technically safe to share closures whenever all upvalues are immutable
            // this is because of a runtime equality check in DUPCLOSURE.
            // however, this results in frequent de-optimization and increases the set of reachable objects, making some temporary objects permanent
            // instead we apply a heuristic: we share closures if they refer to top-level upvalues, or closures that refer to top-level upvalues
            // this will only de-optimize (outside of fenv changes) if top level code is executed twice with different results.
            if (uv->functionDepth != 0 || uv->loopDepth != 0)
            {
                AstExprFunction* uf = ul->init ? ul->init->as<AstExprFunction>() : nullptr;
                if (!uf)
                    return false;

                if (uf != func && !shouldShareClosure(uf))
                    return false;
            }
        }

        return true;
    }

    void compileExprFunction(AstExprFunction* expr, uint8_t target)
    {
        RegScope rs(this);

        const Function* f = functions.find(expr);
        LUAU_ASSERT(f);

        // when the closure has upvalues we'll use this to create the closure at runtime
        // when the closure has no upvalues, we use constant closures that technically don't rely on the child function list
        // however, it's still important to add the child function because debugger relies on the function hierarchy when setting breakpoints
        int16_t pid = bytecode.addChildFunction(f->id);
        if (pid < 0)
            CompileError::raise(expr->location, "Exceeded closure limit; simplify the code to compile");

        // we use a scratch vector to reduce allocations; this is safe since compileExprFunction is not reentrant
        captures.clear();
        captures.reserve(f->upvals.size());

        for (AstLocal* uv : f->upvals)
        {
            LUAU_ASSERT(uv->functionDepth < expr->functionDepth);

            if (int reg = getLocalReg(uv); reg >= 0)
            {
                // note: we can't check if uv is an upvalue in the current frame because inlining can migrate from upvalues to locals
                Variable* ul = variables.find(uv);
                bool immutable = !ul || !ul->written;

                captures.push_back({immutable ? LCT_VAL : LCT_REF, uint8_t(reg)});
            }
            else if (const Constant* uc = locstants.find(uv); uc && uc->type != Constant::Type_Unknown)
            {
                // inlining can result in an upvalue capture of a constant, in which case we can't capture without a temporary register
                uint8_t reg = allocReg(expr, 1u);
                compileExprConstant(expr, uc, reg);

                captures.push_back({LCT_VAL, reg});
            }
            else
            {
                LUAU_ASSERT(uv->functionDepth < expr->functionDepth - 1);

                // get upvalue from parent frame
                // note: this will add uv to the current upvalue list if necessary
                uint8_t uid = getUpval(uv);

                captures.push_back({LCT_UPVAL, uid});
            }
        }

        // Optimization: when closure has no upvalues, or upvalues are safe to share, instead of allocating it every time we can share closure
        // objects (this breaks assumptions about function identity which can lead to setfenv not working as expected, so we disable this when it
        // is used)
        int16_t shared = -1;

        if (options.optimizationLevel >= 1 && shouldShareClosure(expr) && !setfenvUsed)
        {
            int32_t cid = bytecode.addConstantClosure(f->id);

            if (cid >= 0 && cid < 32768)
                shared = int16_t(cid);
        }

        if (shared < 0)
            bytecode.addDebugRemark("allocation: closure with %d upvalues", int(captures.size()));

        if (shared >= 0)
            bytecode.emitAD(LOP_DUPCLOSURE, target, shared);
        else
            bytecode.emitAD(LOP_NEWCLOSURE, target, pid);

        for (const Capture& c : captures)
        {
            bytecode.emitABC(LOP_CAPTURE, uint8_t(c.type), c.data, 0);
        }
    }

    void compileClassDeclaration(AstStatClass* decl)
    {
        LUAU_ASSERT(FFlag::DebugLuauUserDefinedClasses);
        // We allocate one register to store the class object.
        auto dest = allocReg(decl, 1u);

        // CLI-194693: We probably need to add something here to prevent:
        //
        //  class Foobar
        //      public foobar
        //      function foobar() end
        //  end
        //
        // ... properties and methods need to share a namespace.

        pushLocal(decl->name, dest, kDefaultAllocPc);
        if (FFlag::LuauExportValueSyntax && decl->exported)
        {
            // we want to eagerly insert into the exported classes map, as the class may be referenced by one of its methods
            ensureExportTable(decl);
            exports.exportedClasses[decl->name] = dest;
        }

        RegScope _(this);

        // The bottom bit of the C slot indicates whether the class is open.
        uint8_t instrCVal = decl->open ? 1u : 0u;

        if (decl->super)
        {
            // If the superclass is already in a local register, we can reference it directly
            int superReg = getExprLocalReg(decl->super);
            uint8_t superDest = allocReg(decl, decl->super && superReg < 0 ? 1u : 0u);

            if (superReg >= 0)
                bytecode.emitABC(LOP_NEWCLASS, dest, uint8_t(superReg), instrCVal);
            else
            {
                compileExpr(decl->super, superDest);
                bytecode.emitABC(LOP_NEWCLASS, dest, superDest, instrCVal);
            }
        }
        else // The range of valid registers is 0-254, so we use 0xFF (255) to indicate the absence of a superclass.
            bytecode.emitABC(LOP_NEWCLASS, dest, kInvalidReg, instrCVal);

        // We want to load the class constant up front, but in order to load
        // the class constant we need to build it first. To avoid a second
        // pass, we start by emitting the LOADKX bytecode and a dummy
        // constant (0xDEADBEEF), which we will patch later once we
        // have added the class constant to the constant table.
        size_t auxOffset = bytecode.emitLabel();
        bytecode.emitAux(0xDEADBEEF);

        BytecodeBuilder::ClassShape shape;
        shape.className = bytecode.addConstantString(sref(decl->name->name));
        checkConstant(shape.className, decl->name->location);

        // We use this as temporary storage while we make all of the closures
        // associated with this particular class. Another option would be to
        // refactor class construction to be more like a function call and take
        // N registers.
        auto temp = allocReg(decl, 1u);

        bool hasExplicitConstructor = false;

        for (const auto& member : decl->members)
        {
            Luau::visit(
                overloaded{
                    [&](const AstClassProperty& prop)
                    {
                        // Properties we only need to store the name, for now.
                        int propNameCid = bytecode.addConstantString(sref(prop.name));
                        checkConstant(propNameCid, prop.nameLocation);
                        shape.propertyNames.emplace_back(propNameCid);
                    },
                    [&](const AstClassMethod& method)
                    {
                        // For a method:
                        //
                        //  function foobar(a, b, c)
                        //  end
                        //
                        // ... in a class declaration, we compile it as if it were
                        // a free floating function, but instead of assigning it to
                        // a local or a global, we use `NEWCLASSMEMBER` to add it to
                        // our class definition.
                        compileExprFunction(method.function, temp);
                        int methodNameCid = bytecode.addConstantString(sref(method.functionName));
                        checkConstant(methodNameCid, method.function->location);
                        shape.methodNames.emplace_back(methodNameCid);
                        bytecode.emitABC(LOP_NEWCLASSMEMBER, dest, 0, temp);
                        bytecode.emitAux(methodNameCid);

                        if (method.functionName == "__init")
                            hasExplicitConstructor = true;
                    }
                },
                member
            );
        }

        // All classes have `__init` methods.
        if (!hasExplicitConstructor)
        {
            int initCid = bytecode.addConstantString(sref(names.getOrAdd("__init")));
            checkConstant(initCid, decl->location);
            shape.methodNames.emplace_back(initCid);
        }

        // Finally, we create the class constant and patch the AUX slot
        // from before.
        int32_t classConst = bytecode.addClassShape(std::move(shape));
        checkConstant(classConst, decl->location);
        bytecode.patchAux(auxOffset, classConst);
    }

    LuauOpcode getUnaryOp(AstExprUnary::Op op)
    {
        switch (op)
        {
        case AstExprUnary::Op::Not:
            return LOP_NOT;

        case AstExprUnary::Op::Minus:
            return LOP_MINUS;

        case AstExprUnary::Op::Len:
            return LOP_LENGTH;

        default:
            LUAU_ASSERT(!"Unexpected unary operation");
            return LOP_NOP;
        }
    }

    LuauOpcode getBinaryOpArith(AstExprBinary::Op op, bool k = false)
    {
        switch (op)
        {
        case AstExprBinary::Add:
            return k ? LOP_ADDK : LOP_ADD;

        case AstExprBinary::Sub:
            return k ? LOP_SUBK : LOP_SUB;

        case AstExprBinary::Mul:
            return k ? LOP_MULK : LOP_MUL;

        case AstExprBinary::Div:
            return k ? LOP_DIVK : LOP_DIV;

        case AstExprBinary::FloorDiv:
            return k ? LOP_IDIVK : LOP_IDIV;

        case AstExprBinary::Mod:
            return k ? LOP_MODK : LOP_MOD;

        case AstExprBinary::Pow:
            return k ? LOP_POWK : LOP_POW;

        default:
            LUAU_ASSERT(!"Unexpected binary operation");
            return LOP_NOP;
        }
    }

    LuauOpcode getJumpOpCompare(AstExprBinary::Op op, bool not_ = false)
    {
        switch (op)
        {
        case AstExprBinary::CompareNe:
            return not_ ? LOP_JUMPIFEQ : LOP_JUMPIFNOTEQ;

        case AstExprBinary::CompareEq:
            return not_ ? LOP_JUMPIFNOTEQ : LOP_JUMPIFEQ;

        case AstExprBinary::CompareLt:
        case AstExprBinary::CompareGt:
            return not_ ? LOP_JUMPIFNOTLT : LOP_JUMPIFLT;

        case AstExprBinary::CompareLe:
        case AstExprBinary::CompareGe:
            return not_ ? LOP_JUMPIFNOTLE : LOP_JUMPIFLE;

        default:
            LUAU_ASSERT(!"Unexpected binary operation");
            return LOP_NOP;
        }
    }

    bool isConstant(AstExpr* node)
    {
        const Constant* cv = constants.find(node);

        return (cv != nullptr) && cv->type != Constant::Type_Unknown;
    }

    bool isConstantTrue(AstExpr* node)
    {
        return Compile::isConstantTrue(constants, node);
    }

    bool isConstantFalse(AstExpr* node)
    {
        return Compile::isConstantFalse(constants, node);
    }

    bool isConstantVector(AstExpr* node)
    {
        const Constant* cv = constants.find(node);

        return (cv != nullptr) && (cv->type == Constant::Type_Vectorf || cv->type == Constant::Type_Vectord);
    }

    bool isConstantInteger(AstExpr* node)
    {
        const Constant* cv = constants.find(node);

        return cv && cv->type == Constant::Type_Integer;
    }

    Constant getConstant(AstExpr* node)
    {
        const Constant* cv = constants.find(node);

        return cv ? *cv : Constant{Constant::Type_Unknown};
    }

    size_t compileCompareJump(AstExprBinary* expr, bool not_ = false)
    {
        RegScope rs(this);

        bool isEq = (expr->op == AstExprBinary::CompareEq || expr->op == AstExprBinary::CompareNe);
        AstExpr* left = expr->left;
        AstExpr* right = expr->right;

        bool operandIsConstant = isConstant(right);
        if (isEq && !operandIsConstant)
        {
            operandIsConstant = isConstant(left);
            if (operandIsConstant)
                std::swap(left, right);
        }

        // disable fast path for vectors and integers because supporting it would require a new opcode
        if (operandIsConstant && (isConstantVector(right) || (FFlag::LuauIntegerType2 && isConstantInteger(right))))
            operandIsConstant = false;

        uint8_t rl = compileExprAuto(left, rs);

        if (isEq && operandIsConstant)
        {
            const Constant* cv = constants.find(right);
            LUAU_ASSERT(cv && cv->type != Constant::Type_Unknown);

            LuauOpcode opc = LOP_NOP;
            int32_t cid = -1;
            uint32_t flip = (expr->op == AstExprBinary::CompareEq) == not_ ? 0x80000000 : 0;

            switch (cv->type)
            {
            case Constant::Type_Nil:
                opc = LOP_JUMPXEQKNIL;
                cid = 0;
                break;

            case Constant::Type_Boolean:
                opc = LOP_JUMPXEQKB;
                cid = cv->valueBoolean;
                break;

            case Constant::Type_Number:
                opc = LOP_JUMPXEQKN;
                cid = getConstantIndex(right);
                break;

            case Constant::Type_String:
                opc = LOP_JUMPXEQKS;
                cid = getConstantIndex(right);
                break;

            default:
                LUAU_ASSERT(!"Unexpected constant type");
            }

            if (cid < 0)
                CompileError::raise(expr->location, "Exceeded constant limit; simplify the code to compile");

            size_t jumpLabel = bytecode.emitLabel();

            bytecode.emitAD(opc, rl, 0);
            bytecode.emitAux(cid | flip);

            return jumpLabel;
        }
        else
        {
            LuauOpcode opc = getJumpOpCompare(expr->op, not_);

            uint8_t rr = compileExprAuto(right, rs);

            size_t jumpLabel = bytecode.emitLabel();

            if (expr->op == AstExprBinary::CompareGt || expr->op == AstExprBinary::CompareGe)
            {
                bytecode.emitAD(opc, rr, 0);
                bytecode.emitAux(rl);
            }
            else
            {
                bytecode.emitAD(opc, rl, 0);
                bytecode.emitAux(rr);
            }

            return jumpLabel;
        }
    }

    int32_t getConstantNumber(AstExpr* node)
    {
        const Constant* c = constants.find(node);

        if (c && c->type == Constant::Type_Number)
        {
            int cid = bytecode.addConstantNumber(c->valueNumber);
            if (cid < 0)
                CompileError::raise(node->location, "Exceeded constant limit; simplify the code to compile");

            return cid;
        }

        return -1;
    }

    int32_t getConstantIndex(AstExpr* node)
    {
        const Constant* c = constants.find(node);

        if (!c || c->type == Constant::Type_Unknown)
            return -1;

        int cid = -1;

        switch (c->type)
        {
        case Constant::Type_Nil:
            cid = bytecode.addConstantNil();
            break;

        case Constant::Type_Boolean:
            cid = bytecode.addConstantBoolean(c->valueBoolean);
            break;

        case Constant::Type_Number:
            cid = bytecode.addConstantNumber(c->valueNumber);
            break;

        case Constant::Type_Integer:
            cid = bytecode.addConstantInteger(c->valueInteger64);
            break;

        case Constant::Type_Vectorf:
            cid = bytecode.addConstantVectorf(c->valueVectorf[0], c->valueVectorf[1], c->valueVectorf[2], c->valueVectorf[3]);
            break;

        case Constant::Type_Vectord:
            cid = bytecode.addConstantVectord(c->valueVectord[0], c->valueVectord[1], c->valueVectord[2], c->valueVectord[3]);
            break;

        case Constant::Type_String:
            cid = bytecode.addConstantString(sref(c->getString()));
            break;

        default:
            LUAU_ASSERT(!"Unexpected constant type");
            return -1;
        }

        if (cid < 0)
            CompileError::raise(node->location, "Exceeded constant limit; simplify the code to compile");

        return cid;
    }

    // compile expr to target temp register
    // if the expr (or not expr if onlyTruth is false) is truthy, jump via skipJump
    // if the expr (or not expr if onlyTruth is false) is falsy, fall through (target isn't guaranteed to be updated in this case)
    // if target is omitted, then the jump behavior is the same - skipJump or fallthrough depending on the truthiness of the expression
    void compileConditionValue(AstExpr* node, const uint8_t* target, std::vector<size_t>& skipJump, bool onlyTruth)
    {
        // Optimization: we don't need to compute constant values
        if (const Constant* cv = constants.find(node); cv && cv->type != Constant::Type_Unknown)
        {
            // note that we only need to compute the value if it's truthy; otherwise we cal fall through
            if (cv->isTruthful() == onlyTruth)
            {
                if (target)
                    compileExprTemp(node, *target);

                skipJump.push_back(bytecode.emitLabel());
                bytecode.emitAD(LOP_JUMP, 0, 0);
            }
            return;
        }

        if (AstExprBinary* expr = node->as<AstExprBinary>())
        {
            switch (expr->op)
            {
            case AstExprBinary::And:
            case AstExprBinary::Or:
            {
                // disambiguation: there's 4 cases (we only need truthy or falsy results based on onlyTruth)
                // onlyTruth = 1: a and b transforms to a ? b : dontcare
                // onlyTruth = 1: a or b transforms to a ? a : b
                // onlyTruth = 0: a and b transforms to !a ? a : b
                // onlyTruth = 0: a or b transforms to !a ? b : dontcare
                if (onlyTruth == (expr->op == AstExprBinary::And))
                {
                    // we need to compile the left hand side, and skip to "dontcare" (aka fallthrough of the entire statement) if it's not the same as
                    // onlyTruth if it's the same then the result of the expression is the right hand side because of this, we *never* care about the
                    // result of the left hand side
                    std::vector<size_t> elseJump;
                    compileConditionValue(expr->left, nullptr, elseJump, !onlyTruth);

                    // fallthrough indicates that we need to compute & return the right hand side
                    // we use compileConditionValue again to process any extra and/or statements directly
                    compileConditionValue(expr->right, target, skipJump, onlyTruth);

                    size_t elseLabel = bytecode.emitLabel();

                    patchJumps(expr, elseJump, elseLabel);
                }
                else
                {
                    // we need to compute the left hand side first; note that we will jump to skipJump if we know the answer
                    compileConditionValue(expr->left, target, skipJump, onlyTruth);

                    // we will fall through if computing the left hand didn't give us an "interesting" result
                    // we still use compileConditionValue to recursively optimize any and/or/compare statements
                    compileConditionValue(expr->right, target, skipJump, onlyTruth);
                }
                return;
            }
            break;

            case AstExprBinary::CompareNe:
            case AstExprBinary::CompareEq:
            case AstExprBinary::CompareLt:
            case AstExprBinary::CompareLe:
            case AstExprBinary::CompareGt:
            case AstExprBinary::CompareGe:
            {
                if (target)
                {
                    // since target is a temp register, we'll initialize it to 1, and then jump if the comparison is true
                    // if the comparison is false, we'll fallthrough and target will still be 1 but target has unspecified value for falsy results
                    // when we only care about falsy values instead of truthy values, the process is the same but with flipped conditionals
                    bytecode.emitABC(LOP_LOADB, *target, onlyTruth ? 1 : 0, 0);
                }

                size_t jumpLabel = compileCompareJump(expr, /* not= */ !onlyTruth);

                skipJump.push_back(jumpLabel);
                return;
            }
            break;

            // fall-through to default path below
            default:;
            }
        }

        if (AstExprUnary* expr = node->as<AstExprUnary>())
        {
            // if we *do* need to compute the target, we'd have to inject "not" ops on every return path
            // this is possible but cumbersome; so for now we only optimize not expression when we *don't* need the value
            if (!target && expr->op == AstExprUnary::Op::Not)
            {
                compileConditionValue(expr->expr, target, skipJump, !onlyTruth);
                return;
            }
        }

        if (AstExprGroup* expr = node->as<AstExprGroup>())
        {
            compileConditionValue(expr->expr, target, skipJump, onlyTruth);
            return;
        }

        RegScope rs(this);
        uint8_t reg;

        if (target)
        {
            reg = *target;
            compileExprTemp(node, reg);
        }
        else
        {
            reg = compileExprAuto(node, rs);
        }

        skipJump.push_back(bytecode.emitLabel());
        bytecode.emitAD(onlyTruth ? LOP_JUMPIF : LOP_JUMPIFNOT, reg, 0);
    }

    // checks if compiling the expression as a condition value generates code that's faster than using compileExpr
    bool isConditionFast(AstExpr* node)
    {
        const Constant* cv = constants.find(node);

        if (cv && cv->type != Constant::Type_Unknown)
            return true;

        if (AstExprBinary* expr = node->as<AstExprBinary>())
        {
            switch (expr->op)
            {
            case AstExprBinary::And:
            case AstExprBinary::Or:
                return true;

            case AstExprBinary::CompareNe:
            case AstExprBinary::CompareEq:
            case AstExprBinary::CompareLt:
            case AstExprBinary::CompareLe:
            case AstExprBinary::CompareGt:
            case AstExprBinary::CompareGe:
                return true;

            default:
                return false;
            }
        }

        if (AstExprGroup* expr = node->as<AstExprGroup>())
            return isConditionFast(expr->expr);

        return false;
    }

    void compileExprAndOr(AstExprBinary* expr, uint8_t target, bool targetTemp)
    {
        bool and_ = (expr->op == AstExprBinary::And);

        RegScope rs(this);

        // Optimization: when left hand side is a constant, we can emit left hand side or right hand side
        if (const Constant* cl = constants.find(expr->left); cl && cl->type != Constant::Type_Unknown)
        {
            compileExpr(and_ == cl->isTruthful() ? expr->right : expr->left, target, targetTemp);
            return;
        }

        // Note: two optimizations below can lead to inefficient codegen when the left hand side is a condition
        if (!isConditionFast(expr->left))
        {
            // Optimization: when right hand side is a local variable, we can use AND/OR
            if (int reg = getExprLocalReg(expr->right); reg >= 0)
            {
                uint8_t lr = compileExprAuto(expr->left, rs);
                uint8_t rr = uint8_t(reg);

                bytecode.emitABC(and_ ? LOP_AND : LOP_OR, target, lr, rr);
                return;
            }

            // Optimization: when right hand side is a constant, we can use ANDK/ORK
            int32_t cid = getConstantIndex(expr->right);

            if (cid >= 0 && cid <= 255)
            {
                uint8_t lr = compileExprAuto(expr->left, rs);

                bytecode.emitABC(and_ ? LOP_ANDK : LOP_ORK, target, lr, uint8_t(cid));
                return;
            }
        }

        // Optimization: if target is a temp register, we can clobber it which allows us to compute the result directly into it
        // If it's not a temp register, then something like `a = a > 1 or a + 2` may clobber `a` while evaluating left hand side, and `a+2` will break
        uint8_t reg = targetTemp ? target : allocReg(expr, 1u);

        std::vector<size_t> skipJump;
        compileConditionValue(expr->left, &reg, skipJump, /* onlyTruth= */ !and_);

        compileExprTemp(expr->right, reg);

        size_t moveLabel = bytecode.emitLabel();

        patchJumps(expr, skipJump, moveLabel);

        if (target != reg)
            bytecode.emitABC(LOP_MOVE, target, reg, 0);
    }

    void compileExprUnary(AstExprUnary* expr, uint8_t target)
    {
        RegScope rs(this);

        // Special case for integer constants, like -1000000000i
        AstExprConstantInteger* cint = expr->expr->as<AstExprConstantInteger>();
        if (FFlag::LuauIntegerType2 && (expr->op == AstExprUnary::Op::Minus) && (cint != nullptr))
        {
            int32_t cid = bytecode.addConstantInteger((int64_t)(~(uint64_t)cint->value + 1));
            if (cid < 0)
                CompileError::raise(expr->location, "Exceeded constant limit; simplify the code to compile");

            emitLoadK(target, cid);
            return;
        }

        uint8_t re = compileExprAuto(expr->expr, rs);

        bytecode.emitABC(getUnaryOp(expr->op), target, re, 0);
    }

    static void unrollConcats(std::vector<AstExpr*>& args)
    {
        for (;;)
        {
            AstExprBinary* be = args.back()->as<AstExprBinary>();

            if (!be || be->op != AstExprBinary::Concat)
                break;

            args.back() = be->left;
            args.push_back(be->right);
        }
    }

    void compileExprBinary(AstExprBinary* expr, uint8_t target, bool targetTemp)
    {
        RegScope rs(this);

        switch (expr->op)
        {
        case AstExprBinary::Add:
        case AstExprBinary::Sub:
        case AstExprBinary::Mul:
        case AstExprBinary::Div:
        case AstExprBinary::FloorDiv:
        case AstExprBinary::Mod:
        case AstExprBinary::Pow:
        {
            int32_t rc = getConstantNumber(expr->right);

            if (rc >= 0 && rc <= 255)
            {
                uint8_t rl = compileExprAuto(expr->left, rs);

                bytecode.emitABC(getBinaryOpArith(expr->op, /* k= */ true), target, rl, uint8_t(rc));

                hintTemporaryExprRegType(expr->left, rl, LBC_TYPE_NUMBER, /* instLength */ 1);
            }
            else
            {
                if (expr->op == AstExprBinary::Sub || expr->op == AstExprBinary::Div)
                {
                    int32_t lc = getConstantNumber(expr->left);

                    if (lc >= 0 && lc <= 255)
                    {
                        uint8_t rr = compileExprAuto(expr->right, rs);
                        LuauOpcode op = (expr->op == AstExprBinary::Sub) ? LOP_SUBRK : LOP_DIVRK;

                        bytecode.emitABC(op, target, uint8_t(lc), uint8_t(rr));

                        hintTemporaryExprRegType(expr->right, rr, LBC_TYPE_NUMBER, /* instLength */ 1);
                        return;
                    }
                }
                else if (options.optimizationLevel >= 2 && (expr->op == AstExprBinary::Add || expr->op == AstExprBinary::Mul))
                {
                    // Optimization: replace k*r with r*k when r is known to be a number (otherwise metamethods may be called)
                    if (LuauBytecodeType* ty = exprTypes.find(expr))
                    {
                        // Note: for vectors, it only makes sense to do for a multiplication as number+vector is an error
                        if (*ty == LBC_TYPE_NUMBER || (*ty == LBC_TYPE_VECTOR && expr->op == AstExprBinary::Mul))
                        {
                            int32_t lc = getConstantNumber(expr->left);

                            if (lc >= 0 && lc <= 255)
                            {
                                uint8_t rr = compileExprAuto(expr->right, rs);

                                bytecode.emitABC(getBinaryOpArith(expr->op, /* k= */ true), target, rr, uint8_t(lc));

                                hintTemporaryExprRegType(expr->right, rr, LBC_TYPE_NUMBER, /* instLength */ 1);
                                return;
                            }
                        }
                    }
                }

                uint8_t rl = compileExprAuto(expr->left, rs);
                uint8_t rr = compileExprAuto(expr->right, rs);

                bytecode.emitABC(getBinaryOpArith(expr->op), target, rl, rr);

                hintTemporaryExprRegType(expr->left, rl, LBC_TYPE_NUMBER, /* instLength */ 1);
                hintTemporaryExprRegType(expr->right, rr, LBC_TYPE_NUMBER, /* instLength */ 1);
            }
        }
        break;

        case AstExprBinary::Concat:
        {
            std::vector<AstExpr*> args = {expr->left, expr->right};

            // unroll the tree of concats down the right hand side to be able to do multiple ops
            unrollConcats(args);

            uint8_t regs = allocReg(expr, unsigned(args.size()));

            for (size_t i = 0; i < args.size(); ++i)
                if (FFlag::LuauCompileConcatTargetTop)
                    compileExprTempTop(args[i], uint8_t(regs + i));
                else
                    compileExprTemp(args[i], uint8_t(regs + i));

            bytecode.emitABC(LOP_CONCAT, target, regs, uint8_t(regs + args.size() - 1));
        }
        break;

        case AstExprBinary::CompareNe:
        case AstExprBinary::CompareEq:
        case AstExprBinary::CompareLt:
        case AstExprBinary::CompareLe:
        case AstExprBinary::CompareGt:
        case AstExprBinary::CompareGe:
        {
            size_t jumpLabel = compileCompareJump(expr);

            // note: this skips over the next LOADB instruction because of "1" in the C slot
            bytecode.emitABC(LOP_LOADB, target, 0, 1);

            size_t thenLabel = bytecode.emitLabel();

            bytecode.emitABC(LOP_LOADB, target, 1, 0);

            patchJump(expr, jumpLabel, thenLabel);
        }
        break;

        case AstExprBinary::And:
        case AstExprBinary::Or:
        {
            compileExprAndOr(expr, target, targetTemp);
        }
        break;

        default:
            LUAU_ASSERT(!"Unexpected binary operation");
        }
    }

    void compileExprIfElseAndOr(bool and_, uint8_t creg, AstExpr* other, uint8_t target)
    {
        int32_t cid = getConstantIndex(other);

        if (cid >= 0 && cid <= 255)
        {
            bytecode.emitABC(and_ ? LOP_ANDK : LOP_ORK, target, creg, uint8_t(cid));
        }
        else
        {
            RegScope rs(this);
            uint8_t oreg = compileExprAuto(other, rs);

            bytecode.emitABC(and_ ? LOP_AND : LOP_OR, target, creg, oreg);
        }
    }

    void compileExprIfElseLocal(AstExprIfElse* expr, uint8_t target, bool targetTemp)
    {
        LUAU_ASSERT(expr->conditionLocal);

        bool skipElse = isConstantTrue(expr->condition);
        size_t oldLocals = localStack.size();
        size_t elseJump = 0;

        {
            RegScope rs(this);

            uint8_t reg = allocReg(expr, 1u);
            uint32_t allocpc = bytecode.getDebugPC();

            compileExprTemp(expr->condition, reg);
            pushLocal(expr->conditionLocal, reg, allocpc);

            if (!skipElse)
            {
                elseJump = bytecode.emitLabel();
                bytecode.emitAD(LOP_JUMPIFNOT, reg, 0);
            }

            compileExpr(expr->trueExpr, target, targetTemp);

            closeLocals(oldLocals);
            popLocals(oldLocals);
        }

        if (skipElse)
            return;

        // Jump over else expression evaluation
        size_t thenLabel = bytecode.emitLabel();
        bytecode.emitAD(LOP_JUMP, 0, 0);

        size_t elseLabel = bytecode.emitLabel();
        compileExpr(expr->falseExpr, target, targetTemp);
        size_t endLabel = bytecode.emitLabel();

        patchJump(expr, elseJump, elseLabel);
        patchJump(expr, thenLabel, endLabel);
    }

    void compileExprIfElse(AstExprIfElse* expr, uint8_t target, bool targetTemp)
    {
        if (FFlag::LuauExperimentalIfLocalSyntax && expr->conditionLocal)
        {
            compileExprIfElseLocal(expr, target, targetTemp);
            return;
        }

        if (isConstant(expr->condition))
        {
            if (isConstantTrue(expr->condition))
            {
                compileExpr(expr->trueExpr, target, targetTemp);
            }
            else
            {
                compileExpr(expr->falseExpr, target, targetTemp);
            }
        }
        else
        {
            // Optimization: convert some if..then..else expressions into and/or when the other side has no side effects and is very cheap to compute
            // if v then v else e => v or e
            // if v then e else v => v and e
            if (int creg = getExprLocalReg(expr->condition); creg >= 0)
            {
                if (creg == getExprLocalReg(expr->trueExpr) && (getExprLocalReg(expr->falseExpr) >= 0 || isConstant(expr->falseExpr)))
                    return compileExprIfElseAndOr(/* and_= */ false, uint8_t(creg), expr->falseExpr, target);
                else if (creg == getExprLocalReg(expr->falseExpr) && (getExprLocalReg(expr->trueExpr) >= 0 || isConstant(expr->trueExpr)))
                    return compileExprIfElseAndOr(/* and_= */ true, uint8_t(creg), expr->trueExpr, target);
            }

            std::vector<size_t> elseJump;
            compileConditionValue(expr->condition, nullptr, elseJump, false);
            compileExpr(expr->trueExpr, target, targetTemp);

            // Jump over else expression evaluation
            size_t thenLabel = bytecode.emitLabel();
            bytecode.emitAD(LOP_JUMP, 0, 0);

            size_t elseLabel = bytecode.emitLabel();
            compileExpr(expr->falseExpr, target, targetTemp);
            size_t endLabel = bytecode.emitLabel();

            patchJumps(expr, elseJump, elseLabel);
            patchJump(expr, thenLabel, endLabel);
        }
    }

    void compileExprInterpString(AstExprInterpString* expr, uint8_t target, bool targetTemp)
    {
        size_t formatCapacity = 0;
        for (AstArray<char> string : expr->strings)
        {
            formatCapacity += string.size + std::count(string.data, string.data + string.size, '%');
        }

        size_t skippedSubExpr = 0;
        for (size_t index = 0; index < expr->expressions.size; ++index)
        {
            const Constant* c = constants.find(expr->expressions.data[index]);
            if (c && c->type == Constant::Type::Type_String)
            {
                formatCapacity += c->stringLength + std::count(c->valueString, c->valueString + c->stringLength, '%');
                skippedSubExpr++;
            }
            else
                formatCapacity += 2; // "%*"
        }

        std::string formatString;
        formatString.reserve(formatCapacity);

        LUAU_ASSERT(expr->strings.size == expr->expressions.size + 1);
        for (size_t idx = 0; idx < expr->strings.size; idx++)
        {
            AstArray<char> string = expr->strings.data[idx];
            escapeAndAppend(formatString, string.data, string.size);

            if (idx < expr->expressions.size)
            {
                const Constant* c = constants.find(expr->expressions.data[idx]);
                if (c && c->type == Constant::Type::Type_String)
                    escapeAndAppend(formatString, c->valueString, c->stringLength);
                else
                    formatString += "%*";
            }
        }

        int32_t formatStringIndex = -1;

        if (formatString.empty())
        {
            formatStringIndex = bytecode.addConstantString({"", 0});
        }
        else
        {
            AstName interned = names.getOrAdd(formatString.c_str(), formatString.size());
            AstArray<const char> formatStringArray{interned.value, formatString.size()};
            formatStringIndex = bytecode.addConstantString(sref(formatStringArray));
        }

        if (formatStringIndex < 0)
            CompileError::raise(expr->location, "Exceeded constant limit; simplify the code to compile");

        RegScope rs(this);

        unsigned int regCount = unsigned(2 + expr->expressions.size - skippedSubExpr);

        // Optimization: have the format call place the result directly into the target to avoid an extra MOVE
        bool targetTop = targetTemp && target == regTop - 1;
        uint8_t baseReg = targetTop ? allocReg(expr, regCount - 1) - 1 : allocReg(expr, regCount);

        emitLoadK(baseReg, formatStringIndex);

        size_t skipped = 0;
        for (size_t index = 0; index < expr->expressions.size; ++index)
        {
            AstExpr* subExpr = expr->expressions.data[index];
            const Constant* c = constants.find(subExpr);
            if (!c || c->type != Constant::Type::Type_String)
                compileExprTempTop(subExpr, uint8_t(baseReg + 2 + index - skipped));
            else
                skipped++;
        }

        BytecodeBuilder::StringRef formatMethod = sref(AstName("format"));

        int32_t formatMethodIndex = bytecode.addConstantString(formatMethod);
        if (formatMethodIndex < 0)
            CompileError::raise(expr->location, "Exceeded constant limit; simplify the code to compile");

        bytecode.emitABC(LOP_NAMECALL, baseReg, baseReg, uint8_t(BytecodeBuilder::getStringHash(formatMethod)));
        bytecode.emitAux(formatMethodIndex);
        bytecode.emitABC(LOP_CALL, baseReg, uint8_t(expr->expressions.size + 2 - skippedSubExpr), 2);
        if (target != baseReg)
            bytecode.emitABC(LOP_MOVE, target, baseReg, 0);
    }

    static uint8_t encodeHashSize(unsigned int hashSize)
    {
        size_t hashSizeLog2 = 0;
        while ((1u << hashSizeLog2) < hashSize)
            hashSizeLog2++;

        return hashSize == 0 ? 0 : uint8_t(hashSizeLog2 + 1);
    }

    void compileExprTable(AstExprTable* expr, uint8_t target, bool targetTemp, uint8_t constructReg)
    {
        // Optimization: if the table is empty, we can compute it directly into the target
        if (expr->items.size == 0)
        {
            TableShape shape = tableShapes[expr];

            bytecode.addDebugRemark("allocation: table hash %d", shape.hashSize);

            bytecode.emitABC(LOP_NEWTABLE, target, encodeHashSize(shape.hashSize), 0);
            bytecode.emitAux(shape.arraySize);
            return;
        }

        unsigned int arraySize = 0;
        unsigned int hashSize = 0;
        unsigned int recordSize = 0;
        unsigned int indexSize = 0;

        for (size_t i = 0; i < expr->items.size; ++i)
        {
            const AstExprTable::Item& item = expr->items.data[i];

            arraySize += (item.kind == AstExprTable::Item::Kind::List);
            hashSize += (item.kind != AstExprTable::Item::Kind::List);
            recordSize += (item.kind == AstExprTable::Item::Kind::Record);
        }

        // Optimization: allocate sequential explicitly specified numeric indices ([1]) as arrays
        if (arraySize == 0 && hashSize > 0)
        {
            for (size_t i = 0; i < expr->items.size; ++i)
            {
                const AstExprTable::Item& item = expr->items.data[i];
                LUAU_ASSERT(item.key); // no list portion => all items have keys

                const Constant* ckey = constants.find(item.key);

                indexSize += (ckey && ckey->type == Constant::Type_Number && ckey->valueNumber == double(indexSize + 1));
            }

            // we only perform the optimization if we don't have any other []-keys
            // technically it's "safe" to do this even if we have other keys, but doing so changes iteration order and may break existing code
            if (hashSize == recordSize + indexSize)
                hashSize = recordSize;
            else
                indexSize = 0;
        }

        int encodedHashSize = encodeHashSize(hashSize);

        RegScope rs(this);

        // Optimization: if target is a temp register, we can clobber it which allows us to compute the result directly into it
        uint8_t reg = targetTemp ? target : allocReg(expr, 1u);

        // flattening operation where we only load the last element
        // this optimizes for tables like: { data = 43, data = "true", data = 9 }
        // this does not optimize for tables such as: { data = 43, data = function() end, data = 9}
        // in this case, we know that data = 9 should be the element, so we can just skip the rest
        InsertionOrderedMap<int32_t, int32_t> lastKeyVal;
        // Optimization: when all items are record fields, use template tables to compile expression
        if (arraySize == 0 && indexSize == 0 && hashSize == recordSize && recordSize >= 1 && recordSize <= BytecodeBuilder::TableShape::kMaxLength)
        {
            BytecodeBuilder::TableShape shape;

            for (size_t i = 0; i < expr->items.size; ++i)
            {
                const AstExprTable::Item& item = expr->items.data[i];
                LUAU_ASSERT(item.kind == AstExprTable::Item::Kind::Record);

                AstExprConstantString* ckey = item.key->as<AstExprConstantString>();
                LUAU_ASSERT(ckey);

                int keyCid = bytecode.addConstantString(sref(ckey->value));
                if (keyCid < 0)
                    CompileError::raise(ckey->location, "Exceeded constant limit; simplify the code to compile");

                int32_t valueCid = getConstantIndex(item.value);
                if (lastKeyVal.contains(keyCid) && lastKeyVal[keyCid] == -1)
                    continue;

                lastKeyVal[keyCid] = valueCid;
            }

            for (auto& [keyCid, valueCid] : lastKeyVal)
            {
                LUAU_ASSERT(shape.length < BytecodeBuilder::TableShape::kMaxLength);

                size_t idx = shape.length;
                shape.keys[idx] = keyCid;

                shape.constants[idx] = valueCid;
                if (valueCid >= 0)
                {
                    shape.hasConstants = true;
                }

                shape.length++;
            }

            int32_t tid = bytecode.addConstantTable(shape);
            if (tid < 0)
                CompileError::raise(expr->location, "Exceeded constant limit; simplify the code to compile");

            bytecode.addDebugRemark("allocation: table template %d", hashSize);

            if (FFlag::DebugLuauUserDefinedClasses && constructReg != kInvalidReg)
            {
                uint32_t fbSlot = bytecode.addConstructSlot(tid);
                bytecode.emitABC(LOP_CONSTRUCT, reg, constructReg, 0);
                bytecode.emitAux(fbSlot);
            }
            else if (tid < 32768)
            {
                bytecode.emitAD(LOP_DUPTABLE, reg, int16_t(tid));
            }
            else
            {
                // must disable duptable constant optimization here, as we're defaulting back to new table
                shape.hasConstants = false;
                lastKeyVal.clear();

                bytecode.emitABC(LOP_NEWTABLE, reg, uint8_t(encodedHashSize), 0);
                bytecode.emitAux(0);
            }
        }
        else
        {
            // Optimization: instead of allocating one extra element when the last element of the table literal is ..., let SETLIST allocate the
            // correct amount of storage
            const AstExprTable::Item* last = expr->items.size > 0 ? &expr->items.data[expr->items.size - 1] : nullptr;

            bool trailingVarargs = last && last->kind == AstExprTable::Item::Kind::List && last->value->is<AstExprVarargs>();
            LUAU_ASSERT(!trailingVarargs || arraySize > 0);

            unsigned int arrayAllocation = arraySize - trailingVarargs + indexSize;

            if (hashSize == 0)
                bytecode.addDebugRemark("allocation: table array %d", arrayAllocation);
            else if (arrayAllocation == 0)
                bytecode.addDebugRemark("allocation: table hash %d", hashSize);
            else
                bytecode.addDebugRemark("allocation: table hash %d array %d", hashSize, arrayAllocation);

            bytecode.emitABC(LOP_NEWTABLE, reg, uint8_t(encodedHashSize), 0);
            bytecode.emitAux(arrayAllocation);
        }

        unsigned int arrayChunkSize = std::min(16u, arraySize);
        uint8_t arrayChunkReg = allocReg(expr, arrayChunkSize);
        unsigned int arrayChunkCurrent = 0;

        unsigned int arrayIndex = 1;
        bool multRet = false;

        for (size_t i = 0; i < expr->items.size; ++i)
        {
            const AstExprTable::Item& item = expr->items.data[i];

            AstExpr* key = item.key;
            AstExpr* value = item.value;

            if (lastKeyVal.size() > 0 && key && key->is<AstExprConstantString>())
            {
                AstExprConstantString* ckey = item.key->as<AstExprConstantString>();
                LUAU_ASSERT(ckey);

                int keyCid = bytecode.addConstantString(sref(ckey->value));
                if (const int32_t* valueCid = lastKeyVal.get(keyCid))
                {
                    // do not generate assignments for constants
                    if (*valueCid >= 0)
                    {
                        continue;
                    }
                }
            }


            // some key/value pairs don't require us to compile the expressions, so we need to setup the line info here
            setDebugLine(value);

            if (options.coverageLevel >= 2)
            {
                bytecode.emitABC(LOP_COVERAGE, 0, 0, 0);
            }

            // flush array chunk on overflow or before hash keys to maintain insertion order
            if (arrayChunkCurrent > 0 && (key || arrayChunkCurrent == arrayChunkSize))
            {
                bytecode.emitABC(LOP_SETLIST, reg, arrayChunkReg, uint8_t(arrayChunkCurrent + 1));
                bytecode.emitAux(arrayIndex);
                arrayIndex += arrayChunkCurrent;
                arrayChunkCurrent = 0;
            }

            // items with a key are set one by one via SETTABLE/SETTABLEKS/SETTABLEN
            if (key)
            {
                RegScope rsi(this);

                LValue lv = compileLValueIndex(reg, key, rsi);
                uint8_t rv = compileExprAuto(value, rsi);

                compileAssign(lv, rv, nullptr);
            }
            // items without a key are set using SETLIST so that we can initialize large arrays quickly
            else
            {
                uint8_t temp = uint8_t(arrayChunkReg + arrayChunkCurrent);

                if (i + 1 == expr->items.size)
                    multRet = compileExprTempMultRet(value, temp);
                else
                    compileExprTempTop(value, temp);

                arrayChunkCurrent++;
            }
        }

        // flush last array chunk; note that this needs multret handling if the last expression was multret
        if (arrayChunkCurrent)
        {
            bytecode.emitABC(LOP_SETLIST, reg, arrayChunkReg, multRet ? 0 : uint8_t(arrayChunkCurrent + 1));
            bytecode.emitAux(arrayIndex);
        }

        if (target != reg)
            bytecode.emitABC(LOP_MOVE, target, reg, 0);
    }

    bool canImport(AstExprGlobal* expr)
    {
        return options.optimizationLevel >= 1 && getGlobalState(globals, expr->name) != Global::Written;
    }

    bool canImportChain(AstExprGlobal* expr)
    {
        return options.optimizationLevel >= 1 && getGlobalState(globals, expr->name) == Global::Default;
    }

    void compileExprIndexName(AstExprIndexName* expr, uint8_t target, bool targetTemp = false)
    {
        setDebugLine(expr); // normally compileExpr sets up line info, but compileExprIndexName can be called directly

        // Optimization: index chains that start from global variables can be compiled into GETIMPORT statement
        AstExprGlobal* importRoot = 0;
        AstExprIndexName* import1 = 0;
        AstExprIndexName* import2 = 0;

        if (AstExprIndexName* index = expr->expr->as<AstExprIndexName>())
        {
            importRoot = index->expr->as<AstExprGlobal>();
            import1 = index;
            import2 = expr;
        }
        else
        {
            importRoot = expr->expr->as<AstExprGlobal>();
            import1 = expr;
        }

        if (importRoot && canImportChain(importRoot))
        {
            int32_t id0 = bytecode.addConstantString(sref(importRoot->name));
            int32_t id1 = bytecode.addConstantString(sref(import1->index));
            int32_t id2 = import2 ? bytecode.addConstantString(sref(import2->index)) : -1;

            if (id0 < 0 || id1 < 0 || (import2 && id2 < 0))
                CompileError::raise(expr->location, "Exceeded constant limit; simplify the code to compile");

            // Note: GETIMPORT encoding is limited to 10 bits per object id component
            if (id0 < 1024 && id1 < 1024 && id2 < 1024)
            {
                uint32_t iid = import2 ? BytecodeBuilder::getImportId(id0, id1, id2) : BytecodeBuilder::getImportId(id0, id1);
                int32_t cid = bytecode.addImport(iid);

                if (cid >= 0 && cid < 32768)
                {
                    bytecode.emitAD(LOP_GETIMPORT, target, int16_t(cid));
                    bytecode.emitAux(iid);
                    return;
                }
            }
        }

        RegScope rs(this);

        uint8_t reg = target;

        if (int localReg = getExprLocalReg(expr->expr); localReg >= 0) // Locals can be indexed directly
            reg = uint8_t(localReg);
        else if (targetTemp && FFlag::LuauCompileMoveElision) // If target is a temp register, we can compute the result directly into it
            reg = compileExprAutoTemp(expr->expr, target);
        else if (targetTemp && !FFlag::LuauCompileMoveElision)
            compileExprTemp(expr->expr, target);
        else
            reg = compileExprAuto(expr->expr, rs);

        setDebugLine(expr->indexLocation);

        BytecodeBuilder::StringRef iname = sref(expr->index);
        int32_t cid = bytecode.addConstantString(iname);
        if (cid < 0)
            CompileError::raise(expr->location, "Exceeded constant limit; simplify the code to compile");

        bytecode.emitABC(LOP_GETTABLEKS, target, reg, uint8_t(BytecodeBuilder::getStringHash(iname)));
        bytecode.emitAux(cid);

        hintTemporaryExprRegType(expr->expr, reg, LBC_TYPE_TABLE, /* instLength */ 2);
    }

    void compileExprIndexExpr(AstExprIndexExpr* expr, uint8_t target)
    {
        RegScope rs(this);

        Constant cv = getConstant(expr->index);

        if (cv.type == Constant::Type_Number && cv.valueNumber >= 1 && cv.valueNumber <= 256 && double(int(cv.valueNumber)) == cv.valueNumber)
        {
            uint8_t i = uint8_t(int(cv.valueNumber) - 1);

            uint8_t rt = compileExprAuto(expr->expr, rs);

            setDebugLine(expr->index);

            bytecode.emitABC(LOP_GETTABLEN, target, rt, i);

            hintTemporaryExprRegType(expr->expr, rt, LBC_TYPE_TABLE, /* instLength */ 1);
        }
        else if (cv.type == Constant::Type_String)
        {
            BytecodeBuilder::StringRef iname = sref(cv.getString());
            int32_t cid = bytecode.addConstantString(iname);
            if (cid < 0)
                CompileError::raise(expr->location, "Exceeded constant limit; simplify the code to compile");

            uint8_t rt = compileExprAuto(expr->expr, rs);

            setDebugLine(expr->index);

            bytecode.emitABC(LOP_GETTABLEKS, target, rt, uint8_t(BytecodeBuilder::getStringHash(iname)));
            bytecode.emitAux(cid);

            hintTemporaryExprRegType(expr->expr, rt, LBC_TYPE_TABLE, /* instLength */ 2);
        }
        else
        {
            uint8_t rt = compileExprAuto(expr->expr, rs);
            uint8_t ri = compileExprAuto(expr->index, rs);

            bytecode.emitABC(LOP_GETTABLE, target, rt, ri);

            hintTemporaryExprRegType(expr->expr, rt, LBC_TYPE_TABLE, /* instLength */ 1);
            hintTemporaryExprRegType(expr->index, ri, LBC_TYPE_NUMBER, /* instLength */ 1);
        }
    }

    void compileExprGlobal(AstExprGlobal* expr, uint8_t target)
    {
        // Optimization: builtin globals can be retrieved using GETIMPORT
        if (canImport(expr))
        {
            int32_t id0 = bytecode.addConstantString(sref(expr->name));
            if (id0 < 0)
                CompileError::raise(expr->location, "Exceeded constant limit; simplify the code to compile");

            // Note: GETIMPORT encoding is limited to 10 bits per object id component
            if (id0 < 1024)
            {
                uint32_t iid = BytecodeBuilder::getImportId(id0);
                int32_t cid = bytecode.addImport(iid);

                if (cid >= 0 && cid < 32768)
                {
                    bytecode.emitAD(LOP_GETIMPORT, target, int16_t(cid));
                    bytecode.emitAux(iid);
                    return;
                }
            }
        }

        BytecodeBuilder::StringRef gname = sref(expr->name);
        int32_t cid = bytecode.addConstantString(gname);
        if (cid < 0)
            CompileError::raise(expr->location, "Exceeded constant limit; simplify the code to compile");

        bytecode.emitABC(LOP_GETGLOBAL, target, 0, uint8_t(BytecodeBuilder::getStringHash(gname)));
        bytecode.emitAux(cid);
    }

    void compileExprConstant(AstExpr* node, const Constant* cv, uint8_t target)
    {
        switch (cv->type)
        {
        case Constant::Type_Nil:
            bytecode.emitABC(LOP_LOADNIL, target, 0, 0);
            break;

        case Constant::Type_Boolean:
            bytecode.emitABC(LOP_LOADB, target, cv->valueBoolean, 0);
            break;

        case Constant::Type_Number:
        {
            double d = cv->valueNumber;

            if (d >= std::numeric_limits<int16_t>::min() && d <= std::numeric_limits<int16_t>::max() && double(int16_t(d)) == d &&
                !(d == 0.0 && signbit(d)))
            {
                // short number encoding: doesn't require a table entry lookup
                bytecode.emitAD(LOP_LOADN, target, int16_t(d));
            }
            else
            {
                // long number encoding: use generic constant path
                int32_t cid = bytecode.addConstantNumber(d);
                if (cid < 0)
                    CompileError::raise(node->location, "Exceeded constant limit; simplify the code to compile");

                emitLoadK(target, cid);
            }
        }
        break;

        case Constant::Type_Integer:
        {
            int64_t l = cv->valueInteger64;

            int32_t cid = bytecode.addConstantInteger(l);
            if (cid < 0)
                CompileError::raise(node->location, "Exceeded constant limit; simplify the code to compile");

            emitLoadK(target, cid);
        }
        break;

        case Constant::Type_Vectorf:
        {
            int32_t cid = bytecode.addConstantVectorf(cv->valueVectorf[0], cv->valueVectorf[1], cv->valueVectorf[2], cv->valueVectorf[3]);
            if (cid < 0)
                CompileError::raise(node->location, "Exceeded constant limit; simplify the code to compile");

            emitLoadK(target, cid);
        }
        break;

        case Constant::Type_Vectord:
        {
            int32_t cid = bytecode.addConstantVectord(cv->valueVectord[0], cv->valueVectord[1], cv->valueVectord[2], cv->valueVectord[3]);
            if (cid < 0)
                CompileError::raise(node->location, "Exceeded constant limit; simplify the code to compile");

            emitLoadK(target, cid);
        }
        break;

        case Constant::Type_String:
        {
            int32_t cid = bytecode.addConstantString(sref(cv->getString()));
            if (cid < 0)
                CompileError::raise(node->location, "Exceeded constant limit; simplify the code to compile");

            emitLoadK(target, cid);
        }
        break;

        default:
            LUAU_ASSERT(!"Unexpected constant type");
        }
    }

    void compileExpr(AstExpr* node, uint8_t target, bool targetTemp = false)
    {
        setDebugLine(node);

        if (options.coverageLevel >= 2 && needsCoverage(node))
        {
            bytecode.emitABC(LOP_COVERAGE, 0, 0, 0);
        }

        // Optimization: if expression has a constant value, we can emit it directly
        if (const Constant* cv = constants.find(node); cv && cv->type != Constant::Type_Unknown)
        {
            compileExprConstant(node, cv, target);
            return;
        }

        if (AstExprGroup* expr = node->as<AstExprGroup>())
        {
            compileExpr(expr->expr, target, targetTemp);
        }
        else if (node->is<AstExprConstantNil>())
        {
            bytecode.emitABC(LOP_LOADNIL, target, 0, 0);
        }
        else if (AstExprConstantBool* expr = node->as<AstExprConstantBool>())
        {
            bytecode.emitABC(LOP_LOADB, target, expr->value, 0);
        }
        else if (AstExprConstantNumber* expr = node->as<AstExprConstantNumber>())
        {
            int32_t cid = bytecode.addConstantNumber(expr->value);
            if (cid < 0)
                CompileError::raise(expr->location, "Exceeded constant limit; simplify the code to compile");

            emitLoadK(target, cid);
        }
        else if (AstExprConstantInteger* expr = node->as<AstExprConstantInteger>())
        {
            int32_t cid = bytecode.addConstantInteger(expr->value);
            if (cid < 0)
                CompileError::raise(expr->location, "Exceeded constant limit; simplify the code to compile");

            emitLoadK(target, cid);
        }
        else if (AstExprConstantString* expr = node->as<AstExprConstantString>())
        {
            int32_t cid = bytecode.addConstantString(sref(expr->value));
            if (cid < 0)
                CompileError::raise(expr->location, "Exceeded constant limit; simplify the code to compile");

            emitLoadK(target, cid);
        }
        else if (AstExprLocal* expr = node->as<AstExprLocal>())
        {
            if (FFlag::LuauExportValueSyntax && expr->local->isExported && !exports.exportedClasses.contains(expr->local) &&
                (!FFlag::LuauOptimizeExportTable || !exports.exportedFunctions.contains(expr->local)))

            {
                BytecodeBuilder::StringRef name = sref(expr->local->name);
                int32_t cid = bytecode.addConstantString(name);
                if (cid < 0)
                    CompileError::raise(expr->location, "Exceeded constant limit; simplify the code to compile");

                if (int tableReg = getLocalReg(&exports.exportTableLocal); tableReg >= 0)
                {
                    bytecode.emitABC(LOP_GETTABLEKS, target, tableReg, uint8_t(BytecodeBuilder::getStringHash(name)));
                    bytecode.emitAux(cid);
                }
                else
                {
                    // we must reuse the target register for the export table lookup
                    uint8_t upval = getUpval(&exports.exportTableLocal);
                    bytecode.emitABC(LOP_GETUPVAL, target, upval, 0);
                    bytecode.emitABC(LOP_GETTABLEKS, target, target, uint8_t(BytecodeBuilder::getStringHash(name)));
                    bytecode.emitAux(cid);
                }
            }
            else
            {
                // note: this can't check expr->upvalue because upvalues may be upgraded to locals during inlining
                if (int reg = getExprLocalReg(expr); reg >= 0)
                {
                    // Optimization: we don't need to move if target happens to be in the same register
                    if (options.optimizationLevel == 0 || target != reg)
                        bytecode.emitABC(LOP_MOVE, target, uint8_t(reg), 0);
                }
                else
                {
                    LUAU_ASSERT(expr->upvalue);
                    uint8_t uid = getUpval(expr->local);

                    bytecode.emitABC(LOP_GETUPVAL, target, uid, 0);
                }
            }
        }
        else if (AstExprGlobal* expr = node->as<AstExprGlobal>())
        {
            compileExprGlobal(expr, target);
        }
        else if (AstExprVarargs* expr = node->as<AstExprVarargs>())
        {
            compileExprVarargs(expr, target, /* targetCount= */ 1);
        }
        else if (AstExprCall* expr = node->as<AstExprCall>())
        {
            // Optimization: when targeting temporary registers, we can compile call in a special mode that doesn't require extra register moves
            if (targetTemp && target == regTop - 1)
                compileExprCall(expr, target, 1, /* targetTop= */ true);
            else
                compileExprCall(expr, target, /* targetCount= */ 1);
        }
        else if (AstExprIndexName* expr = node->as<AstExprIndexName>())
        {
            compileExprIndexName(expr, target, targetTemp);
        }
        else if (AstExprIndexExpr* expr = node->as<AstExprIndexExpr>())
        {
            compileExprIndexExpr(expr, target);
        }
        else if (AstExprFunction* expr = node->as<AstExprFunction>())
        {
            compileExprFunction(expr, target);
        }
        else if (AstExprTable* expr = node->as<AstExprTable>())
        {
            compileExprTable(expr, target, targetTemp, kInvalidReg);
        }
        else if (AstExprUnary* expr = node->as<AstExprUnary>())
        {
            compileExprUnary(expr, target);
        }
        else if (AstExprBinary* expr = node->as<AstExprBinary>())
        {
            compileExprBinary(expr, target, targetTemp);
        }
        else if (AstExprTypeAssertion* expr = node->as<AstExprTypeAssertion>())
        {
            compileExpr(expr->expr, target, targetTemp);
        }
        else if (AstExprIfElse* expr = node->as<AstExprIfElse>())
        {
            compileExprIfElse(expr, target, targetTemp);
        }
        else if (AstExprInterpString* interpString = node->as<AstExprInterpString>())
        {
            compileExprInterpString(interpString, target, targetTemp);
        }
        else if (AstExprInstantiate* expr = node->as<AstExprInstantiate>())
        {
            compileExpr(expr->expr, target, targetTemp);
        }
        else
        {
            LUAU_ASSERT(!"Unknown expression type");
        }
    }

    void compileExprTemp(AstExpr* node, uint8_t target)
    {
        return compileExpr(node, target, /* targetTemp= */ true);
    }

    uint8_t compileExprAutoTemp(AstExpr* node, uint8_t target, uint8_t forbiddenSourceReg = kInvalidReg)
    {
        LUAU_ASSERT(FFlag::LuauCompileMoveElision);

        if (options.optimizationLevel == 0)
        {
            compileExprTemp(node, target);
            return target;
        }

        size_t marker = bytecode.emitLabel();
        compileExprTemp(node, target);

        // If only a single instruction was generated, check if it's a move of a register that we can elide
        if (bytecode.emitLabel() == marker + 1)
        {
            unsigned insn = bytecode.lastInstruction();

            if (LUAU_INSN_OP(insn) == LOP_MOVE && LUAU_INSN_A(insn) == target)
            {
                uint8_t src = LUAU_INSN_B(insn);

                // Captured registers might get modified while the operation is lowered and rely on the move copy to prevent that
                if (!regCaptured[src] && src != forbiddenSourceReg)
                {
                    bytecode.undoEmit(LOP_MOVE);
                    target = src;
                }
            }
        }

        return target;
    }

    uint8_t compileExprAuto(AstExpr* node, RegScope&)
    {
        // Optimization: directly return locals instead of copying them to a temporary
        // This can affect side-effect ordering on the register if it is captured
        if (int reg = getExprLocalReg(node); reg >= 0)
            return uint8_t(reg);

        // note: the register is owned by the parent scope
        uint8_t reg = allocReg(node, 1u);

        if (FFlag::LuauCompileMoveElision)
        {
            return compileExprAutoTemp(node, reg);
        }
        else
        {
            compileExprTemp(node, reg);

            return reg;
        }
    }

    void compileExprSide(AstExpr* node)
    {
        // Optimization: some expressions never carry side effects so we don't need to emit any code
        if (node->is<AstExprLocal>() || node->is<AstExprGlobal>() || node->is<AstExprVarargs>() || node->is<AstExprFunction>() || isConstant(node))
            return;

        // note: the remark is omitted for calls as it's fairly noisy due to inlining
        if (!node->is<AstExprCall>())
            bytecode.addDebugRemark("expression only compiled for side effects");

        RegScope rsi(this);
        compileExprAuto(node, rsi);
    }

    // initializes target..target+targetCount-1 range using expression
    // if expression is a call/vararg, we assume it returns all values, otherwise we fill the rest with nil
    // assumes target register range can be clobbered and is at the top of the register space if targetTop = true
    void compileExprTempN(AstExpr* node, uint8_t target, uint8_t targetCount, bool targetTop)
    {
        // we assume that target range is at the top of the register space and can be clobbered
        // this is what allows us to compile the last call expression - if it's a call - using targetTop=true
        LUAU_ASSERT(!targetTop || unsigned(target + targetCount) == regTop);

        // LOP_CALL/LOP_GETVARARGS encoding uses 255 to signal a multret
        if (targetCount == 255)
            CompileError::raise(node->location, "Exceeded result count limit; simplify the code to compile");

        if (AstExprCall* expr = node->as<AstExprCall>())
        {
            compileExprCall(expr, target, targetCount, targetTop);
        }
        else if (AstExprVarargs* expr = node->as<AstExprVarargs>())
        {
            compileExprVarargs(expr, target, targetCount);
        }
        else
        {
            compileExprTemp(node, target);

            for (size_t i = 1; i < targetCount; ++i)
                bytecode.emitABC(LOP_LOADNIL, uint8_t(target + i), 0, 0);
        }
    }

    // initializes target..target+targetCount-1 range using expressions from the list
    // if list has fewer expressions, and last expression is multret, we assume it returns the rest of the values
    // if list has fewer expressions, and last expression isn't multret, we fill the rest with nil
    // assumes target register range can be clobbered and is at the top of the register space if targetTop = true
    void compileExprListTemp(const AstArray<AstExpr*>& list, uint8_t target, uint8_t targetCount, bool targetTop)
    {
        // we assume that target range is at the top of the register space and can be clobbered
        // this is what allows us to compile the last call expression - if it's a call - using targetTop=true
        LUAU_ASSERT(!targetTop || unsigned(target + targetCount) == regTop);

        if (list.size == targetCount)
        {
            for (size_t i = 0; i < list.size; ++i)
                compileExprTemp(list.data[i], uint8_t(target + i));
        }
        else if (list.size > targetCount)
        {
            for (size_t i = 0; i < targetCount; ++i)
                compileExprTemp(list.data[i], uint8_t(target + i));

            // evaluate extra expressions for side effects
            for (size_t i = targetCount; i < list.size; ++i)
                compileExprSide(list.data[i]);
        }
        else if (list.size > 0)
        {
            for (size_t i = 0; i < list.size - 1; ++i)
                compileExprTemp(list.data[i], uint8_t(target + i));

            compileExprTempN(list.data[list.size - 1], uint8_t(target + list.size - 1), uint8_t(targetCount - (list.size - 1)), targetTop);
        }
        else
        {
            for (size_t i = 0; i < targetCount; ++i)
                bytecode.emitABC(LOP_LOADNIL, uint8_t(target + i), 0, 0);
        }
    }

    struct LValue
    {
        enum Kind
        {
            Kind_Local,
            Kind_Upvalue,
            Kind_Global,
            Kind_IndexName,
            Kind_IndexNumber,
            Kind_IndexExpr,
        };

        Kind kind;
        uint8_t reg; // register for local (Local) or table (Index*)
        uint8_t upval;
        uint8_t index;  // register for index in IndexExpr
        uint8_t number; // index-1 (0-255) in IndexNumber
        BytecodeBuilder::StringRef name;
        Location location;
    };

    LValue compileLValueIndex(uint8_t reg, AstExpr* index, RegScope& rs)
    {
        Constant cv = getConstant(index);

        if (cv.type == Constant::Type_Number && cv.valueNumber >= 1 && cv.valueNumber <= 256 && double(int(cv.valueNumber)) == cv.valueNumber)
        {
            LValue result = {LValue::Kind_IndexNumber};
            result.reg = reg;
            result.number = uint8_t(int(cv.valueNumber) - 1);
            result.location = index->location;

            return result;
        }
        else if (cv.type == Constant::Type_String)
        {
            LValue result = {LValue::Kind_IndexName};
            result.reg = reg;
            result.name = sref(cv.getString());
            result.location = index->location;

            return result;
        }
        else
        {
            LValue result = {LValue::Kind_IndexExpr};
            result.reg = reg;
            result.index = compileExprAuto(index, rs);
            result.location = index->location;

            return result;
        }
    }

    LValue compileLValue(AstExpr* node, RegScope& rs)
    {
        setDebugLine(node);

        if (AstExprLocal* expr = node->as<AstExprLocal>())
        {
            if (FFlag::LuauExportValueSyntax && expr->local->isExported &&
                (!FFlag::LuauOptimizeExportTable || !exports.exportedFunctions.contains(expr->local)))
            {
                uint8_t tableReg = getExportTableReg(node);

                LValue result = {LValue::Kind_IndexName};
                result.reg = tableReg;
                result.name = sref(expr->local->name);
                result.location = node->location;

                return result;
            }

            // note: this can't check expr->upvalue because upvalues may be upgraded to locals during inlining
            if (int reg = getExprLocalReg(expr); reg >= 0)
            {
                LValue result = {LValue::Kind_Local};
                result.reg = uint8_t(reg);
                result.location = node->location;

                return result;
            }
            else
            {
                LUAU_ASSERT(expr->upvalue);

                LValue result = {LValue::Kind_Upvalue};
                result.upval = getUpval(expr->local);
                result.location = node->location;

                return result;
            }
        }
        else if (AstExprGlobal* expr = node->as<AstExprGlobal>())
        {
            LValue result = {LValue::Kind_Global};
            result.name = sref(expr->name);
            result.location = node->location;

            return result;
        }
        else if (AstExprIndexName* expr = node->as<AstExprIndexName>())
        {
            LValue result = {LValue::Kind_IndexName};
            result.reg = compileExprAuto(expr->expr, rs);
            result.name = sref(expr->index);
            result.location = node->location;

            return result;
        }
        else if (AstExprIndexExpr* expr = node->as<AstExprIndexExpr>())
        {
            uint8_t reg = compileExprAuto(expr->expr, rs);

            return compileLValueIndex(reg, expr->index, rs);
        }
        else
        {
            LUAU_ASSERT(!"Unknown assignment expression");

            return LValue();
        }
    }

    void compileLValueUse(const LValue& lv, uint8_t reg, bool set, AstExpr* targetExpr)
    {
        setDebugLine(lv.location);

        switch (lv.kind)
        {
        case LValue::Kind_Local:
            if (set)
                bytecode.emitABC(LOP_MOVE, lv.reg, reg, 0);
            else
                bytecode.emitABC(LOP_MOVE, reg, lv.reg, 0);
            break;

        case LValue::Kind_Upvalue:
            bytecode.emitABC(set ? LOP_SETUPVAL : LOP_GETUPVAL, reg, lv.upval, 0);
            break;

        case LValue::Kind_Global:
        {
            int32_t cid = bytecode.addConstantString(lv.name);
            if (cid < 0)
                CompileError::raise(lv.location, "Exceeded constant limit; simplify the code to compile");

            bytecode.emitABC(set ? LOP_SETGLOBAL : LOP_GETGLOBAL, reg, 0, uint8_t(BytecodeBuilder::getStringHash(lv.name)));
            bytecode.emitAux(cid);
        }
        break;

        case LValue::Kind_IndexName:
        {
            int32_t cid = bytecode.addConstantString(lv.name);
            if (cid < 0)
                CompileError::raise(lv.location, "Exceeded constant limit; simplify the code to compile");

            bytecode.emitABC(set ? LOP_SETTABLEKS : LOP_GETTABLEKS, reg, lv.reg, uint8_t(BytecodeBuilder::getStringHash(lv.name)));
            bytecode.emitAux(cid);

            if (targetExpr)
            {
                if (AstExprIndexName* targetExprIndexName = targetExpr->as<AstExprIndexName>())
                    hintTemporaryExprRegType(targetExprIndexName->expr, lv.reg, LBC_TYPE_TABLE, /* instLength */ 2);
            }
        }
        break;

        case LValue::Kind_IndexNumber:
            bytecode.emitABC(set ? LOP_SETTABLEN : LOP_GETTABLEN, reg, lv.reg, lv.number);

            if (targetExpr)
            {
                if (AstExprIndexExpr* targetExprIndexExpr = targetExpr->as<AstExprIndexExpr>())
                    hintTemporaryExprRegType(targetExprIndexExpr->expr, lv.reg, LBC_TYPE_TABLE, /* instLength */ 1);
            }
            break;

        case LValue::Kind_IndexExpr:
            bytecode.emitABC(set ? LOP_SETTABLE : LOP_GETTABLE, reg, lv.reg, lv.index);

            if (targetExpr)
            {
                if (AstExprIndexExpr* targetExprIndexExpr = targetExpr->as<AstExprIndexExpr>())
                {
                    hintTemporaryExprRegType(targetExprIndexExpr->expr, lv.reg, LBC_TYPE_TABLE, /* instLength */ 1);
                    hintTemporaryExprRegType(targetExprIndexExpr->index, lv.index, LBC_TYPE_NUMBER, /* instLength */ 1);
                }
            }
            break;

        default:
            LUAU_ASSERT(!"Unknown lvalue kind");
        }
    }

    void compileAssign(const LValue& lv, uint8_t source, AstExpr* targetExpr)
    {
        compileLValueUse(lv, source, /* set= */ true, targetExpr);
    }

    AstExprLocal* getExprLocal(AstExpr* node)
    {
        return unwrapExprOfType<AstExprLocal>(node);
    }

    int getExprLocalReg(AstExpr* node)
    {
        if (AstExprLocal* expr = getExprLocal(node))
        {
            // note: this can't check expr->upvalue because upvalues may be upgraded to locals during inlining
            Local* l = locals.find(expr->local);

            return l && l->allocated ? l->reg : -1;
        }
        else
            return -1;
    }

    bool isStatBreak(AstStat* node)
    {
        if (AstStatBlock* stat = node->as<AstStatBlock>())
            return stat->body.size == 1 && stat->body.data[0]->is<AstStatBreak>();

        return node->is<AstStatBreak>();
    }

    AstStatContinue* extractStatContinue(AstStatBlock* block)
    {
        if (block->body.size == 1)
            return block->body.data[0]->as<AstStatContinue>();
        else
            return nullptr;
    }

    void compileStatIfLocal(AstStatIf* stat)
    {
        LUAU_ASSERT(stat->conditionLocal);

        bool thenTerminates = alwaysTerminates(stat->thenbody);
        bool skipElse = isConstantTrue(stat->condition);
        size_t oldLocals = localStack.size();
        size_t elseJump = 0;

        {
            RegScope rs(this);

            uint8_t reg = allocReg(stat, 1u);
            uint32_t allocpc = bytecode.getDebugPC();

            compileExprTemp(stat->condition, reg);
            pushLocal(stat->conditionLocal, reg, allocpc);

            if (!skipElse)
            {
                elseJump = bytecode.emitLabel();
                bytecode.emitAD(LOP_JUMPIFNOT, reg, 0);
            }

            compileStat(stat->thenbody);

            if (!thenTerminates)
                closeLocals(oldLocals);

            popLocals(oldLocals);
        }

        if (skipElse)
            return;

        if (stat->elsebody)
        {
            if (thenTerminates)
            {
                size_t elseLabel = bytecode.emitLabel();
                compileStat(stat->elsebody);
                patchJump(stat, elseJump, elseLabel);
            }
            else
            {
                size_t thenLabel = bytecode.emitLabel();
                bytecode.emitAD(LOP_JUMP, 0, 0);

                size_t elseLabel = bytecode.emitLabel();
                compileStat(stat->elsebody);

                size_t endLabel = bytecode.emitLabel();
                patchJump(stat, elseJump, elseLabel);
                patchJump(stat, thenLabel, endLabel);
            }
        }
        else
        {
            size_t endLabel = bytecode.emitLabel();
            patchJump(stat, elseJump, endLabel);
        }
    }

    void compileStatIf(AstStatIf* stat)
    {
        // Optimization: condition is always false => we only need the else body
        if (isConstantFalse(stat->condition))
        {
            if (stat->elsebody)
                compileStat(stat->elsebody);
            return;
        }

        // Optimization: condition is always false but isn't a constant => we only need the else body and condition's side effects
        if (AstExprBinary* cand = stat->condition->as<AstExprBinary>(); cand && cand->op == AstExprBinary::And && isConstantFalse(cand->right))
        {
            compileExprSide(cand->left);
            if (stat->elsebody)
                compileStat(stat->elsebody);
            return;
        }

        if (FFlag::LuauExperimentalIfLocalSyntax && stat->conditionLocal)
        {
            compileStatIfLocal(stat);
            return;
        }

        // Optimization: body is a "break" statement with no "else" => we can directly break out of the loop in "then" case
        if (!stat->elsebody && isStatBreak(stat->thenbody) && !areLocalsCaptured(loops.back().localOffset))
        {
            // fallthrough = continue with the loop as usual
            std::vector<size_t> elseJump;
            compileConditionValue(stat->condition, nullptr, elseJump, true);

            for (size_t jump : elseJump)
                loopJumps.push_back({LoopJump::Break, jump});
            return;
        }

        AstStatContinue* continueStatement = extractStatContinue(stat->thenbody);

        // Optimization: body is a "continue" statement with no "else" => we can directly continue in "then" case
        if (!stat->elsebody && continueStatement != nullptr && !areLocalsCaptured(loops.back().localOffsetContinue))
        {
            // track continue statement for repeat..until validation (validateContinueUntil)
            if (!loops.back().continueUsed)
                loops.back().continueUsed = continueStatement;

            // fallthrough = proceed with the loop body as usual
            std::vector<size_t> elseJump;
            compileConditionValue(stat->condition, nullptr, elseJump, true);

            for (size_t jump : elseJump)
                loopJumps.push_back({LoopJump::Continue, jump});
            return;
        }

        std::vector<size_t> elseJump;
        compileConditionValue(stat->condition, nullptr, elseJump, false);

        compileStat(stat->thenbody);

        if (stat->elsebody && elseJump.size() > 0)
        {
            // we don't need to skip past "else" body if "then" ends with return/break/continue
            // this is important because, if "else" also ends with return, we may *not* have any statement to skip to!
            if (alwaysTerminates(stat->thenbody))
            {
                size_t elseLabel = bytecode.emitLabel();

                compileStat(stat->elsebody);

                patchJumps(stat, elseJump, elseLabel);
            }
            else
            {
                size_t thenLabel = bytecode.emitLabel();

                bytecode.emitAD(LOP_JUMP, 0, 0);

                size_t elseLabel = bytecode.emitLabel();

                compileStat(stat->elsebody);

                size_t endLabel = bytecode.emitLabel();

                patchJumps(stat, elseJump, elseLabel);
                patchJump(stat, thenLabel, endLabel);
            }
        }
        else
        {
            size_t endLabel = bytecode.emitLabel();

            patchJumps(stat, elseJump, endLabel);
        }
    }

    void compileStatWhile(AstStatWhile* stat)
    {
        // Optimization: condition is always false => there's no loop!
        if (isConstantFalse(stat->condition))
            return;

        size_t oldJumps = loopJumps.size();
        size_t oldLocals = localStack.size();

        loops.push_back({oldLocals, oldLocals, nullptr});
        hasLoops = true;

        size_t loopLabel = bytecode.emitLabel();

        std::vector<size_t> elseJump;
        compileConditionValue(stat->condition, nullptr, elseJump, false);

        compileStat(stat->body);

        size_t contLabel = bytecode.emitLabel();

        size_t backLabel = bytecode.emitLabel();

        setDebugLine(stat->condition);

        // Note: this is using JUMPBACK, not JUMP, since JUMPBACK is interruptible and we want all loops to have at least one interruptible
        // instruction
        bytecode.emitAD(LOP_JUMPBACK, 0, 0);

        size_t endLabel = bytecode.emitLabel();

        patchJump(stat, backLabel, loopLabel);
        patchJumps(stat, elseJump, endLabel);

        patchLoopJumps(stat, oldJumps, endLabel, contLabel);
        loopJumps.resize(oldJumps);

        loops.pop_back();
    }

    void compileStatRepeat(AstStatRepeat* stat)
    {
        size_t oldJumps = loopJumps.size();
        size_t oldLocals = localStack.size();

        loops.push_back({oldLocals, oldLocals, nullptr});
        hasLoops = true;

        size_t loopLabel = bytecode.emitLabel();

        // note: we "inline" compileStatBlock here so that we can close/pop locals after evaluating condition
        // this is necessary because condition can access locals declared inside the repeat..until body
        AstStatBlock* body = stat->body;

        RegScope rs(this);

        bool continueValidated = false;
        size_t conditionLocals = 0;

        for (size_t i = 0; i < body->body.size; ++i)
        {
            if (FFlag::LuauCompileReuseLocalRegs)
                popStatLocals(body, body->body.data[i], oldLocals);

            compileStat(body->body.data[i]);

            // continue statement inside the repeat..until loop should not close upvalues defined directly in the loop body before it
            // this is because the upvalues defined inside the loop body may be captured by a closure defined in the until
            // expression that continue will jump to.
            // but any locals defined in nested blocks or after first continue (which performs validateContinueUntil) do have to be closed
            if (!loops.back().continueUsed)
                loops.back().localOffsetContinue = localStack.size();

            // if continue was called from this statement, any local defined after this in the loop body should not be accessed by until condition
            // it is sufficient to check this condition once, as if this holds for the first continue, it must hold for all subsequent continues.
            if (loops.back().continueUsed && !continueValidated)
            {
                validateContinueUntil(loops.back().continueUsed, stat->condition, body, i + 1);
                continueValidated = true;
                conditionLocals = localStack.size();
            }
        }

        // if continue was used, some locals might not have had their initialization completed
        // the lifetime of these locals has to end before the condition is executed
        // because referencing skipped locals is not possible from the condition, this earlier closure doesn't affect upvalues
        if (continueValidated)
        {
            // if continueValidated is set, it means we have visited at least one body node and size > 0
            setDebugLineEnd(body->body.data[body->body.size - 1]);

            closeLocals(conditionLocals);

            popLocals(conditionLocals);
        }

        size_t contLabel = bytecode.emitLabel();

        size_t endLabel;

        setDebugLine(stat->condition);

        if (isConstantTrue(stat->condition))
        {
            closeLocals(oldLocals);

            endLabel = bytecode.emitLabel();
        }
        else
        {
            std::vector<size_t> skipJump;
            compileConditionValue(stat->condition, nullptr, skipJump, true);

            // we close locals *after* we compute loop conditionals because during computation of condition it's (in theory) possible that user code
            // mutates them
            closeLocals(oldLocals);

            size_t backLabel = bytecode.emitLabel();

            // Note: this is using JUMPBACK, not JUMP, since JUMPBACK is interruptible and we want all loops to have at least one interruptible
            // instruction
            bytecode.emitAD(LOP_JUMPBACK, 0, 0);

            size_t skipLabel = bytecode.emitLabel();

            // we need to close locals *again* after the loop ends because the first closeLocals would be jumped over on the last iteration
            closeLocals(oldLocals);

            endLabel = bytecode.emitLabel();

            patchJump(stat, backLabel, loopLabel);
            patchJumps(stat, skipJump, skipLabel);
        }

        popLocals(oldLocals);

        patchLoopJumps(stat, oldJumps, endLabel, contLabel);
        loopJumps.resize(oldJumps);

        loops.pop_back();
    }

    void compileInlineReturn(AstStatReturn* stat, bool fallthrough)
    {
        setDebugLine(stat); // normally compileStat sets up line info, but compileInlineReturn can be called directly

        InlineFrame frame = inlineFrames.back();

        compileExprListTemp(stat->list, frame.target, frame.targetCount, /* targetTop= */ false);

        closeLocals(frame.localOffset);

        size_t jumpLabel = bytecode.emitLabel();
        bytecode.emitAD(LOP_JUMP, 0, 0);

        inlineFrames.back().returnJumps.push_back(jumpLabel);
    }

    void compileStatReturn(AstStatReturn* stat)
    {
        // LOP_RETURN encoding uses 255 to signal a multret
        if (stat->list.size >= 255)
            CompileError::raise(stat->location, "Exceeded return count limit; simplify the code to compile");

        RegScope rs(this);

        uint8_t temp = 0;
        bool consecutive = false;
        bool multRet = false;

        // Optimization: return locals directly instead of copying them into a temporary
        // this is very important for a single return value and occasionally effective for multiple values
        if (int reg = stat->list.size > 0 ? getExprLocalReg(stat->list.data[0]) : -1; reg >= 0)
        {
            temp = uint8_t(reg);
            consecutive = true;

            for (size_t i = 1; i < stat->list.size; ++i)
                if (getExprLocalReg(stat->list.data[i]) != int(temp + i))
                {
                    consecutive = false;
                    break;
                }
        }

        // Optimization: try to re-adjust the non-consecutive sequence into a consecutive prefix and extra moves
        if (FFlag::LuauCompileReuseLocalRegs && !consecutive && stat->list.size > 1)
        {
            AstArray<AstExpr*>& list = stat->list;
            bool allLocals = true;

            for (AstExpr* expr : list)
            {
                if (getExprLocalReg(expr) == -1)
                    allLocals = false;
            }

            if (allLocals)
            {
                uint8_t base = uint8_t(getExprLocalReg(list.data[0]));
                size_t prefix = 1;

                while (prefix < list.size && getExprLocalReg(list.data[prefix]) == int(base + prefix))
                    prefix++;

                if (base + prefix == regTop)
                {
                    uint8_t tail = allocReg(stat, unsigned(list.size - prefix));
                    LUAU_ASSERT(tail == base + prefix);

                    for (size_t i = prefix; i < list.size; i++)
                        bytecode.emitABC(LOP_MOVE, uint8_t(base + i), uint8_t(getExprLocalReg(list.data[i])), 0);

                    temp = base;
                    consecutive = true;
                }
            }
        }

        if (FFlag::LuauCompileMoveElision && !consecutive && stat->list.size == 1 && !isExprMultRet(stat->list.data[0]))
        {
            // Single expression return might be able to elide the extra move into the fresh register
            temp = allocReg(stat, 1u);
            temp = compileExprAutoTemp(stat->list.data[0], temp);
        }
        else if (!consecutive && stat->list.size > 0)
        {
            temp = allocReg(stat, unsigned(stat->list.size));

            // Note: if the last element is a function call or a vararg specifier, then we need to somehow return all values that that call returned
            for (size_t i = 0; i < stat->list.size; ++i)
                if (i + 1 == stat->list.size)
                    multRet = compileExprTempMultRet(stat->list.data[i], uint8_t(temp + i));
                else
                    compileExprTempTop(stat->list.data[i], uint8_t(temp + i));
        }

        closeLocals(0);

        if (multRet)
            hasMultiRet = true;

        bytecode.emitABC(LOP_RETURN, uint8_t(temp), multRet ? 0 : uint8_t(stat->list.size + 1), 0);
    }

    bool areLocalsRedundant(AstStatLocal* stat)
    {
        // Extra expressions may have side effects
        if (stat->values.size > stat->vars.size)
            return false;

        for (AstLocal* local : stat->vars)
        {
            Variable* v = variables.find(local);
            if (!v || !v->constant)
                return false;

            if (FFlag::LuauExportValueSyntax && local->isExported && (!FFlag::LuauOptimizeExportTable || exports.exportedTableCid == -1))
            {
                // exported locals must be written to the export table
                return false;
            }
        }

        return true;
    }

    void compileStatLocal(AstStatLocal* stat)
    {
        // Optimization: we don't need to allocate and assign const locals, since their uses will be constant-folded
        if (options.optimizationLevel >= 1 && options.debugLevel <= 1 && areLocalsRedundant(stat))
            return;

        // Optimization: for 1-1 local assignments, we can reuse the register *if* neither local is mutated
        if (options.optimizationLevel >= 1 && stat->vars.size == 1 && stat->values.size == 1)
        {
            if (AstExprLocal* re = getExprLocal(stat->values.data[0]))
            {
                Variable* lv = variables.find(stat->vars.data[0]);
                Variable* rv = variables.find(re->local);

                if (int reg = getExprLocalReg(re);
                    reg >= 0 && (!lv || !lv->written) && (!rv || !rv->written) && !stat->vars.data[0]->isExported && !re->local->isExported)
                {
                    pushLocal(stat->vars.data[0], uint8_t(reg), kDefaultAllocPc);
                    return;
                }
            }

            // Declaration of a local that becomes the inlined function result can use the return target register
            if (FFlag::LuauCompileMoveElision && !inlineFrames.empty() && inlineFrames.back().resultLocal == stat->vars.data[0])
            {
                compileExprTemp(stat->values.data[0], inlineFrames.back().target);

                // TODO: unreachable
                // evaluate extra expressions for side effects
                for (size_t i = stat->vars.size; i < stat->values.size; ++i)
                    compileExprSide(stat->values.data[i]);

                pushLocal(stat->vars.data[0], inlineFrames.back().target, kDefaultAllocPc);
                return;
            }

            if (FFlag::LuauCompileReuseLocalRegs)
            {
                uint8_t target = kInvalidReg;
                if (tryReuseLocalReg(stat->vars.data[0], target))
                {
                    uint32_t allocpc = bytecode.getDebugPC();
                    compileExprTemp(stat->values.data[0], target);
                    pushLocal(stat->vars.data[0], target, allocpc);
                    return;
                }
            }
        }

        // note: allocReg in this case allocates into parent block register - note that we don't have RegScope here
        uint8_t vars = allocReg(stat, unsigned(stat->vars.size));
        uint32_t allocpc = bytecode.getDebugPC();

        compileExprListTemp(stat->values, vars, uint8_t(stat->vars.size), /* targetTop= */ true);

        for (size_t i = 0; i < stat->vars.size; ++i)
        {
            AstLocal* local = stat->vars.data[i];
            Variable* localVariable = variables.find(local);
            if (FFlag::LuauExportValueSyntax && local->isExported &&
                (!FFlag::LuauOptimizeExportTable || !localVariable->constant || exports.exportedTableCid == -1))
            {
                ensureExportTable(stat);

                int32_t cid = bytecode.addConstantString(sref(local->name));
                if (cid < 0)
                    CompileError::raise(local->location, "Exceeded constant limit; simplify the code to compile");

                uint8_t tableReg = getExportTableReg(stat);
                bytecode.emitABC(LOP_SETTABLEKS, uint8_t(vars + i), tableReg, uint8_t(BytecodeBuilder::getStringHash(sref(local->name))));
                bytecode.emitAux(cid);
            }
            else
            {
                pushLocal(local, uint8_t(vars + i), allocpc);
            }
        }
    }

    bool tryCompileUnrolledFor(AstStatFor* stat, int thresholdBase, int thresholdMaxBoost)
    {
        Constant one = {Constant::Type_Number};
        one.valueNumber = 1.0;

        Constant fromc = getConstant(stat->from);
        Constant toc = getConstant(stat->to);
        Constant stepc = stat->step ? getConstant(stat->step) : one;

        int tripCount = (fromc.type == Constant::Type_Number && toc.type == Constant::Type_Number && stepc.type == Constant::Type_Number)
                            ? getTripCount(fromc.valueNumber, toc.valueNumber, stepc.valueNumber)
                            : -1;

        if (tripCount < 0)
        {
            bytecode.addDebugRemark("loop unroll failed: invalid iteration count");
            return false;
        }

        if (FFlag::LuauCompileLoopUnrollZero && tripCount == 0)
        {
            bytecode.addDebugRemark("loop unroll succeeded: empty loop");
            return true;
        }

        if (tripCount > thresholdBase)
        {
            bytecode.addDebugRemark("loop unroll failed: too many iterations (%d)", tripCount);
            return false;
        }

        if (Variable* lv = variables.find(stat->var); lv && lv->written)
        {
            bytecode.addDebugRemark("loop unroll failed: mutable loop variable");
            return false;
        }

        AstLocal* var = stat->var;
        uint64_t costModel = modelCost(stat->body, &var, 1, builtins, constants);

        // we use a dynamic cost threshold that's based on the fixed limit boosted by the cost advantage we gain due to unrolling
        bool varc = true;
        int unrolledCost = computeCost(costModel, &varc, 1) * tripCount;
        int baselineCost = (computeCost(costModel, nullptr, 0) + 1) * tripCount;
        int unrollProfit = (unrolledCost == 0) ? thresholdMaxBoost : std::min(thresholdMaxBoost, 100 * baselineCost / unrolledCost);

        int threshold = thresholdBase * unrollProfit / 100;

        if (unrolledCost > threshold)
        {
            bytecode.addDebugRemark(
                "loop unroll failed: too expensive (iterations %d, cost %d, profit %.2fx)", tripCount, unrolledCost, double(unrollProfit) / 100
            );
            return false;
        }

        bytecode.addDebugRemark("loop unroll succeeded (iterations %d, cost %d, profit %.2fx)", tripCount, unrolledCost, double(unrollProfit) / 100);

        compileUnrolledFor(stat, tripCount, fromc.valueNumber, stepc.valueNumber);
        return true;
    }

    void compileUnrolledFor(AstStatFor* stat, int tripCount, double from, double step)
    {
        AstLocal* var = stat->var;

        size_t oldLocals = localStack.size();
        size_t oldJumps = loopJumps.size();

        loops.push_back({oldLocals, oldLocals, nullptr});

        // record changes on the first iteration to capture the pre-loop state
        exprChanges.clear();
        localChanges.clear();

        for (int iv = 0; iv < tripCount; ++iv)
        {
            // we need to re-fold constants in the loop body with the new value; this reuses computed constant values elsewhere in the tree
            locstants[var].type = Constant::Type_Number;
            locstants[var].valueNumber = from + iv * step;

            if (iv == 0)
                foldConstants(
                    constants,
                    variables,
                    locstants,
                    builtinsFold,
                    builtinsFoldLibraryK,
                    options.vectorPrecision == 1,
                    options.libraryMemberConstantCb,
                    stat,
                    names,
                    tableConstants,
                    &exprChanges,
                    &localChanges
                );
            else
                foldConstants(
                    constants,
                    variables,
                    locstants,
                    builtinsFold,
                    builtinsFoldLibraryK,
                    options.vectorPrecision == 1,
                    options.libraryMemberConstantCb,
                    stat,
                    names,
                    tableConstants
                );


            size_t iterJumps = loopJumps.size();

            compileStat(stat->body);

            // all continue jumps need to go to the next iteration
            size_t contLabel = bytecode.emitLabel();

            for (size_t i = iterJumps; i < loopJumps.size(); ++i)
                if (loopJumps[i].type == LoopJump::Continue)
                    patchJump(stat, loopJumps[i].label, contLabel);
        }

        // all break jumps need to go past the loop
        size_t endLabel = bytecode.emitLabel();

        for (size_t i = oldJumps; i < loopJumps.size(); ++i)
            if (loopJumps[i].type == LoopJump::Break)
                patchJump(stat, loopJumps[i].label, endLabel);

        loopJumps.resize(oldJumps);

        loops.pop_back();

        // clean up fold state in case we need to recompile - normally we compile the loop body once, but due to inlining we may need to do it again
        locstants[var].type = Constant::Type_Unknown;

        Compile::undoChanges(constants, exprChanges);
        Compile::undoChanges(locstants, localChanges);
    }

    void compileStatFor(AstStatFor* stat)
    {
        RegScope rs(this);

        // Optimization: small loops can be unrolled when it is profitable
        if (options.optimizationLevel >= 2 && isConstant(stat->to) && isConstant(stat->from) && (!stat->step || isConstant(stat->step)))
            if (tryCompileUnrolledFor(stat, FInt::LuauCompileLoopUnrollThreshold, FInt::LuauCompileLoopUnrollThresholdMaxBoost))
                return;

        size_t oldLocals = localStack.size();
        size_t oldJumps = loopJumps.size();

        loops.push_back({oldLocals, oldLocals, nullptr});
        hasLoops = true;

        // register layout: limit, step, index
        uint8_t regs = allocReg(stat, 3u);

        // if the iteration index is assigned from within the loop, we need to protect the internal index from the assignment
        // to do that, we will copy the index into an actual local variable on each iteration
        // this makes sure the code inside the loop can't interfere with the iteration process (other than modifying the table we're iterating
        // through)
        uint8_t varreg = regs + 2;
        uint32_t varregallocpc = bytecode.getDebugPC();

        if (Variable* il = variables.find(stat->var); il && il->written)
            varreg = allocReg(stat, 1u);

        compileExprTemp(stat->from, uint8_t(regs + 2));
        compileExprTemp(stat->to, uint8_t(regs + 0));

        if (stat->step)
            compileExprTemp(stat->step, uint8_t(regs + 1));
        else
            bytecode.emitABC(LOP_LOADN, uint8_t(regs + 1), 1, 0);

        size_t forLabel = bytecode.emitLabel();

        bytecode.emitAD(LOP_FORNPREP, regs, 0);

        size_t loopLabel = bytecode.emitLabel();

        if (varreg != regs + 2)
            bytecode.emitABC(LOP_MOVE, varreg, regs + 2, 0);

        pushLocal(stat->var, varreg, varregallocpc);

        compileStat(stat->body);

        closeLocals(oldLocals);
        popLocals(oldLocals);

        setDebugLine(stat);

        size_t contLabel = bytecode.emitLabel();

        size_t backLabel = bytecode.emitLabel();

        bytecode.emitAD(LOP_FORNLOOP, regs, 0);

        size_t endLabel = bytecode.emitLabel();

        patchJump(stat, forLabel, endLabel);
        patchJump(stat, backLabel, loopLabel);

        patchLoopJumps(stat, oldJumps, endLabel, contLabel);
        loopJumps.resize(oldJumps);

        loops.pop_back();
    }

    void compileStatForIn(AstStatForIn* stat)
    {
        RegScope rs(this);

        size_t oldLocals = localStack.size();
        size_t oldJumps = loopJumps.size();

        loops.push_back({oldLocals, oldLocals, nullptr});
        hasLoops = true;

        // register layout: generator, state, index, variables...
        uint8_t regs = allocReg(stat, 3u);

        // this puts initial values of (generator, state, index) into the loop registers
        compileExprListTemp(stat->values, regs, 3, /* targetTop= */ true);

        // note that we reserve at least 2 variables; this allows our fast path to assume that we need 2 variables instead of 1 or 2
        uint8_t vars = allocReg(stat, std::max(unsigned(stat->vars.size), 2u));
        LUAU_ASSERT(vars == regs + 3);
        uint32_t varsallocpc = bytecode.getDebugPC();

        LuauOpcode skipOp = LOP_FORGPREP;

        // Optimization: when we iterate via pairs/ipairs, we generate special bytecode that optimizes the traversal using internal iteration index
        // These instructions dynamically check if generator is equal to next/inext and bail out
        // They assume that the generator produces 2 variables, which is why we allocate at least 2 above (see vars assignment)
        if (options.optimizationLevel >= 1 && stat->vars.size <= 2)
        {
            if (stat->values.size == 1 && stat->values.data[0]->is<AstExprCall>())
            {
                Builtin builtin = getBuiltin(stat->values.data[0]->as<AstExprCall>()->func, globals, variables);

                if (builtin.isGlobal("ipairs")) // for .. in ipairs(t)
                    skipOp = LOP_FORGPREP_INEXT;
                else if (builtin.isGlobal("pairs")) // for .. in pairs(t)
                    skipOp = LOP_FORGPREP_NEXT;
            }
            else if (stat->values.size == 2 && (!getfenvUsed && !setfenvUsed))
            {
                Builtin builtin = getBuiltin(stat->values.data[0], globals, variables);

                if (builtin.isGlobal("next")) // for .. in next,t
                    skipOp = LOP_FORGPREP_NEXT;
            }
        }

        // first iteration jumps into FORGLOOP instruction, but for ipairs/pairs it does extra preparation that makes the cost of an extra instruction
        // worthwhile
        size_t skipLabel = bytecode.emitLabel();

        bytecode.emitAD(skipOp, regs, 0);

        size_t loopLabel = bytecode.emitLabel();

        for (size_t i = 0; i < stat->vars.size; ++i)
            pushLocal(stat->vars.data[i], uint8_t(vars + i), varsallocpc);

        compileStat(stat->body);

        closeLocals(oldLocals);
        popLocals(oldLocals);

        setDebugLine(stat);

        size_t contLabel = bytecode.emitLabel();

        size_t backLabel = bytecode.emitLabel();

        // FORGLOOP uses aux to encode variable count and fast path flag for ipairs traversal in the high bit
        bytecode.emitAD(LOP_FORGLOOP, regs, 0);
        bytecode.emitAux((skipOp == LOP_FORGPREP_INEXT ? 0x80000000 : 0) | uint32_t(stat->vars.size));

        size_t endLabel = bytecode.emitLabel();

        patchJump(stat, skipLabel, backLabel);
        patchJump(stat, backLabel, loopLabel);

        patchLoopJumps(stat, oldJumps, endLabel, contLabel);
        loopJumps.resize(oldJumps);

        loops.pop_back();
    }

    struct Assignment
    {
        LValue lvalue;

        uint8_t conflictReg = kInvalidReg;
        uint8_t valueReg = kInvalidReg;
    };

    // This function analyzes assignments and marks assignment conflicts: cases when a variable is assigned on lhs
    // but subsequently used on the rhs, assuming assignments are performed in order. Note that it's also possible
    // for a variable to conflict on the lhs, if it's used in an lvalue expression after it's assigned.
    // When conflicts are found, Assignment::conflictReg is allocated and that's where assignment is performed instead,
    // until the final fixup in compileStatAssign. Assignment::valueReg is allocated by compileStatAssign as well.
    //
    // Per Lua manual, section 3.3.3 (Assignments), the proper assignment order is only guaranteed to hold for syntactic access:
    //
    //     Note that this guarantee covers only accesses syntactically inside the assignment statement. If a function or a metamethod called
    //     during the assignment changes the value of a variable, Lua gives no guarantees about the order of that access.
    //
    // As such, we currently don't check if an assigned local is captured, which may mean it gets reassigned during a function call.
    void resolveAssignConflicts(AstStat* stat, std::vector<Assignment>& vars, const AstArray<AstExpr*>& values)
    {
        struct Visitor : AstVisitor
        {
            Compiler* self;

            std::bitset<256> conflict;
            std::bitset<256> assigned;

            Visitor(Compiler* self)
                : self(self)
            {
            }

            bool visit(AstExprLocal* node) override
            {
                int reg = self->getLocalReg(node->local);

                if (reg >= 0 && assigned[reg])
                    conflict[reg] = true;

                return true;
            }
        };

        Visitor visitor(this);

        // mark any registers that are used *after* assignment as conflicting

        // first we go through assignments to locals, since they are performed before assignments to other l-values
        for (size_t i = 0; i < vars.size(); ++i)
        {
            const LValue& li = vars[i].lvalue;

            if (li.kind == LValue::Kind_Local)
            {
                if (i < values.size)
                    values.data[i]->visit(&visitor);

                visitor.assigned[li.reg] = true;
            }
        }

        // and now we handle all other l-values
        for (size_t i = 0; i < vars.size(); ++i)
        {
            const LValue& li = vars[i].lvalue;

            if (li.kind != LValue::Kind_Local && i < values.size)
                values.data[i]->visit(&visitor);
        }

        // mark any registers used in trailing expressions as conflicting as well
        for (size_t i = vars.size(); i < values.size; ++i)
            values.data[i]->visit(&visitor);

        // mark any registers used on left hand side that are also assigned anywhere as conflicting
        // this is order-independent because we evaluate all right hand side arguments into registers before doing table assignments
        for (const Assignment& var : vars)
        {
            const LValue& li = var.lvalue;

            if ((li.kind == LValue::Kind_IndexName || li.kind == LValue::Kind_IndexNumber || li.kind == LValue::Kind_IndexExpr) &&
                visitor.assigned[li.reg])
                visitor.conflict[li.reg] = true;

            if (li.kind == LValue::Kind_IndexExpr && visitor.assigned[li.index])
                visitor.conflict[li.index] = true;
        }

        // for any conflicting var, we need to allocate a temporary register where the assignment is performed, so that we can move the value later
        for (Assignment& var : vars)
        {
            const LValue& li = var.lvalue;

            if (li.kind == LValue::Kind_Local && visitor.conflict[li.reg])
                var.conflictReg = allocReg(stat, 1u);
        }
    }

    void compileStatAssign(AstStatAssign* stat)
    {
        RegScope rs(this);

        // Optimization: one to one assignments don't require complex conflict resolution machinery
        if (stat->vars.size == 1 && stat->values.size == 1)
        {
            LValue var = compileLValue(stat->vars.data[0], rs);

            // Optimization: assign to locals directly
            if (var.kind == LValue::Kind_Local)
            {
                compileExpr(stat->values.data[0], var.reg);
            }
            else
            {
                uint8_t reg = compileExprAuto(stat->values.data[0], rs);

                setDebugLine(stat->vars.data[0]);
                compileAssign(var, reg, stat->vars.data[0]);
            }
            return;
        }

        // compute all l-values: note that this doesn't assign anything yet but it allocates registers and computes complex expressions on the
        // left hand side - for example, in "a[expr] = foo" expr will get evaluated here
        std::vector<Assignment> vars(stat->vars.size);

        for (size_t i = 0; i < stat->vars.size; ++i)
            vars[i].lvalue = compileLValue(stat->vars.data[i], rs);

        // perform conflict resolution: if any expression refers to a local that is assigned before evaluating it, we assign to a temporary
        // register after this, vars[i].conflictReg is set for locals that need to be assigned in the second pass
        resolveAssignConflicts(stat, vars, stat->values);

        // compute rhs into (mostly) fresh registers
        // note that when the lhs assignment is a local, we evaluate directly into that register
        // this is possible because resolveAssignConflicts renamed conflicting locals into temporaries
        // after this, vars[i].valueReg is set to a register with the value for *all* vars, but some have already been assigned
        for (size_t i = 0; i < stat->vars.size && i < stat->values.size; ++i)
        {
            AstExpr* value = stat->values.data[i];

            if (i + 1 == stat->values.size && stat->vars.size > stat->values.size)
            {
                // allocate a consecutive range of regs for all remaining vars and compute everything into temps
                // note, this also handles trailing nils
                unsigned rest = unsigned(stat->vars.size - stat->values.size + 1);
                uint8_t temp = allocReg(stat, rest);

                compileExprTempN(value, temp, uint8_t(rest), /* targetTop= */ true);

                for (size_t j = i; j < stat->vars.size; ++j)
                    vars[j].valueReg = uint8_t(temp + (j - i));
            }
            else
            {
                Assignment& var = vars[i];

                // if target is a local, use compileExpr directly to target
                if (var.lvalue.kind == LValue::Kind_Local)
                {
                    var.valueReg = (var.conflictReg == kInvalidReg) ? var.lvalue.reg : var.conflictReg;

                    compileExpr(stat->values.data[i], var.valueReg);
                }
                else
                {
                    var.valueReg = compileExprAuto(stat->values.data[i], rs);
                }
            }
        }

        // compute expressions with side effects
        for (size_t i = stat->vars.size; i < stat->values.size; ++i)
            compileExprSide(stat->values.data[i]);

        // almost done... let's assign everything left to right, noting that locals were either written-to directly, or will be written-to in a
        // separate pass to avoid conflicts
        size_t varPos = 0;
        for (const Assignment& var : vars)
        {
            LUAU_ASSERT(var.valueReg != kInvalidReg);

            if (var.lvalue.kind != LValue::Kind_Local)
            {
                setDebugLine(var.lvalue.location);

                if (varPos < stat->vars.size)
                    compileAssign(var.lvalue, var.valueReg, stat->vars.data[varPos]);
                else
                    compileAssign(var.lvalue, var.valueReg, nullptr);
            }

            varPos++;
        }

        // all regular local writes are done by the prior loops by computing result directly into target, so this just handles conflicts OR
        // local copies from temporary registers in multret context, since in that case we have to allocate consecutive temporaries
        for (const Assignment& var : vars)
        {
            if (var.lvalue.kind == LValue::Kind_Local && var.valueReg != var.lvalue.reg)
                bytecode.emitABC(LOP_MOVE, var.lvalue.reg, var.valueReg, 0);
        }
    }

    void compileStatCompoundAssign(AstStatCompoundAssign* stat)
    {
        RegScope rs(this);

        LValue var = compileLValue(stat->var, rs);

        // Optimization: assign to locals directly
        uint8_t target = (var.kind == LValue::Kind_Local) ? var.reg : allocReg(stat, 1u);

        switch (stat->op)
        {
        case AstExprBinary::Add:
        case AstExprBinary::Sub:
        case AstExprBinary::Mul:
        case AstExprBinary::Div:
        case AstExprBinary::FloorDiv:
        case AstExprBinary::Mod:
        case AstExprBinary::Pow:
        {
            if (var.kind != LValue::Kind_Local)
                compileLValueUse(var, target, /* set= */ false, stat->var);

            int32_t rc = getConstantNumber(stat->value);

            if (rc >= 0 && rc <= 255)
            {
                bytecode.emitABC(getBinaryOpArith(stat->op, /* k= */ true), target, target, uint8_t(rc));
            }
            else
            {
                uint8_t rr = compileExprAuto(stat->value, rs);

                bytecode.emitABC(getBinaryOpArith(stat->op), target, target, rr);

                if (var.kind != LValue::Kind_Local)
                    hintTemporaryRegType(stat->var, target, LBC_TYPE_NUMBER, /* instLength */ 1);

                hintTemporaryExprRegType(stat->value, rr, LBC_TYPE_NUMBER, /* instLength */ 1);
            }
        }
        break;

        case AstExprBinary::Concat:
        {
            std::vector<AstExpr*> args = {stat->value};

            // unroll the tree of concats down the right hand side to be able to do multiple ops
            unrollConcats(args);

            uint8_t regs = allocReg(stat, unsigned(1 + args.size()));

            compileLValueUse(var, regs, /* set= */ false, stat->var);

            for (size_t i = 0; i < args.size(); ++i)
                if (FFlag::LuauCompileConcatTargetTop)
                    compileExprTempTop(args[i], uint8_t(regs + 1 + i));
                else
                    compileExprTemp(args[i], uint8_t(regs + 1 + i));

            bytecode.emitABC(LOP_CONCAT, target, regs, uint8_t(regs + args.size()));
        }
        break;

        default:
            LUAU_ASSERT(!"Unexpected compound assignment operation");
        }

        if (var.kind != LValue::Kind_Local)
            compileAssign(var, target, stat->var);
    }

    void compileStatFunction(AstStatFunction* stat)
    {
        // Optimization: compile value expression directly into target local register
        if (int reg = getExprLocalReg(stat->name); reg >= 0)
        {
            compileExpr(stat->func, uint8_t(reg));
            return;
        }

        RegScope rs(this);
        uint8_t reg = allocReg(stat, 1u);

        compileExprTemp(stat->func, reg);

        LValue var = compileLValue(stat->name, rs);
        compileAssign(var, reg, stat->name);
    }

    void compileStat(AstStat* node)
    {
        setDebugLine(node);

        if (options.coverageLevel >= 1 && needsCoverage(node))
        {
            bytecode.emitABC(LOP_COVERAGE, 0, 0, 0);
        }

        if (AstStatBlock* stat = node->as<AstStatBlock>())
        {
            RegScope rs(this);

            size_t oldLocals = localStack.size();
            if (FFlag::LuauExportValueSyntax)
                blockDepth++;

            bool terminatesEarly = false;

            for (size_t i = 0; i < stat->body.size; ++i)
            {
                AstStat* bodyStat = stat->body.data[i];

                if (FFlag::LuauCompileReuseLocalRegs)
                    popStatLocals(stat, bodyStat, 0u);

                compileStat(bodyStat);

                if (FFlag::LuauCompileCleanBlockDeadClose)
                {
                    if (alwaysTerminates(bodyStat))
                    {
                        terminatesEarly = true;
                        break;
                    }
                }
                else
                {
                    if (alwaysTerminates(bodyStat))
                        break;
                }
            }

            if (FFlag::LuauExportValueSyntax)
                blockDepth--;

            if (FFlag::LuauCompileCleanBlockDeadClose)
            {
                if (!terminatesEarly)
                    closeLocals(oldLocals);
            }
            else
            {
                closeLocals(oldLocals);
            }

            popLocals(oldLocals);
        }
        else if (AstStatIf* stat = node->as<AstStatIf>())
        {
            compileStatIf(stat);
        }
        else if (AstStatWhile* stat = node->as<AstStatWhile>())
        {
            compileStatWhile(stat);
        }
        else if (AstStatRepeat* stat = node->as<AstStatRepeat>())
        {
            compileStatRepeat(stat);
        }
        else if (node->is<AstStatBreak>())
        {
            LUAU_ASSERT(!loops.empty());

            // before exiting out of the loop, we need to close all local variables that were captured in closures since loop start
            // normally they are closed by the enclosing blocks, including the loop block, but we're skipping that here
            closeLocals(loops.back().localOffset);

            size_t label = bytecode.emitLabel();

            bytecode.emitAD(LOP_JUMP, 0, 0);

            loopJumps.push_back({LoopJump::Break, label});
        }
        else if (AstStatContinue* stat = node->as<AstStatContinue>())
        {
            LUAU_ASSERT(!loops.empty());

            // track continue statement for repeat..until validation (validateContinueUntil)
            if (!loops.back().continueUsed)
                loops.back().continueUsed = stat;

            // before continuing, we need to close all local variables that were captured in closures since loop start
            // normally they are closed by the enclosing blocks, including the loop block, but we're skipping that here
            closeLocals(loops.back().localOffsetContinue);

            size_t label = bytecode.emitLabel();

            bytecode.emitAD(LOP_JUMP, 0, 0);

            loopJumps.push_back({LoopJump::Continue, label});
        }
        else if (AstStatReturn* stat = node->as<AstStatReturn>())
        {
            if (options.optimizationLevel >= 2 && !inlineFrames.empty())
                compileInlineReturn(stat, /* fallthrough= */ false);
            else
                compileStatReturn(stat);
        }
        else if (AstStatExpr* stat = node->as<AstStatExpr>())
        {
            // Optimization: since we don't need to read anything from the stack, we can compile the call to not return anything which saves register
            // moves
            if (AstExprCall* expr = stat->expr->as<AstExprCall>())
            {
                uint8_t target = uint8_t(regTop);

                compileExprCall(expr, target, /* targetCount= */ 0);
            }
            else
            {
                compileExprSide(stat->expr);
            }
        }
        else if (AstStatLocal* stat = node->as<AstStatLocal>())
        {
            if (FFlag::LuauExportValueSyntax)
            {
                for (auto& local : stat->vars)
                {
                    checkExportedLocal(local, stat->location);
                }
            }
            compileStatLocal(stat);
        }
        else if (AstStatFor* stat = node->as<AstStatFor>())
        {
            compileStatFor(stat);
        }
        else if (AstStatForIn* stat = node->as<AstStatForIn>())
        {
            compileStatForIn(stat);
        }
        else if (AstStatAssign* stat = node->as<AstStatAssign>())
        {
            compileStatAssign(stat);
        }
        else if (AstStatCompoundAssign* stat = node->as<AstStatCompoundAssign>())
        {
            compileStatCompoundAssign(stat);
        }
        else if (AstStatFunction* stat = node->as<AstStatFunction>())
        {
            compileStatFunction(stat);
        }
        else if (AstStatLocalFunction* stat = node->as<AstStatLocalFunction>())
        {
            if (FFlag::LuauExportValueSyntax && !FFlag::LuauOptimizeExportTable && stat->name->isExported)
            {
                checkExportedLocal(stat->name, stat->location);

                ensureExportTable(stat);

                RegScope rs(this);
                uint8_t var = allocReg(stat, 1u);
                compileExprFunction(stat->func, var);

                int32_t cid = bytecode.addConstantString(sref(stat->name->name));
                if (cid < 0)
                    CompileError::raise(stat->name->location, "Exceeded constant limit; simplify the code to compile");

                uint8_t tableReg = getExportTableReg(stat);
                bytecode.emitABC(LOP_SETTABLEKS, var, tableReg, uint8_t(BytecodeBuilder::getStringHash(sref(stat->name->name))));
                bytecode.emitAux(cid);
            }
            else
            {
                uint8_t var = kInvalidReg;

                if (FFlag::LuauCompileReuseLocalRegs)
                {
                    if (!tryReuseLocalReg(stat->name, var))
                        var = allocReg(stat, 1u);
                }
                else
                {
                    var = allocReg(stat, 1u);
                }

                pushLocal(stat->name, var, kDefaultAllocPc);
                compileExprFunction(stat->func, var);

                Local& l = locals[stat->name];

                // we *have* to pushLocal before we compile the function, since the function may refer to the local as an upvalue
                // however, this means the debugpc for the local is at an instruction where the local value hasn't been computed yet
                // to fix this we just move the debugpc after the local value is established
                l.debugpc = bytecode.getDebugPC();
            }
        }
        else if (AstStatTypeAlias* alias = node->as<AstStatTypeAlias>())
        {
            if (FFlag::LuauExportedTypesParticipateInScc && alias->exported && atTopLevel())
                exports.hasTypeExports = true;
        }
        else if (node->is<AstStatTypeFunction>())
        {
            // do nothing
        }
        else if (FFlag::DebugLuauUserDefinedClasses && node->is<AstStatClass>())
        {
            compileClassDeclaration(node->as<AstStatClass>());
        }
        else
        {
            LUAU_ASSERT(!"Unknown statement type");
        }
    }

    void validateContinueUntil(AstStat* cont, AstExpr* condition, AstStatBlock* body, size_t start)
    {
        UndefinedLocalVisitor visitor(this);

        for (size_t i = start; i < body->body.size; ++i)
        {
            if (AstStatLocal* stat = body->body.data[i]->as<AstStatLocal>())
            {
                for (AstLocal* local : stat->vars)
                    visitor.locals.insert(local);
            }
            else if (AstStatLocalFunction* stat = body->body.data[i]->as<AstStatLocalFunction>())
            {
                visitor.locals.insert(stat->name);
            }
        }

        condition->visit(&visitor);

        if (visitor.undef)
            CompileError::raise(
                condition->location,
                "Local %s used in the repeat..until condition is undefined because continue statement on line %d jumps over it",
                visitor.undef->name.value,
                cont->location.begin.line + 1
            );
    }

    void gatherConstUpvals(AstExprFunction* func)
    {
        ConstUpvalueVisitor visitor(this);
        func->body->visit(&visitor);

        for (AstLocal* local : visitor.upvals)
            getUpval(local);
    }

    void pushLocal(AstLocal* local, uint8_t reg, uint32_t allocpc)
    {
        if (localStack.size() >= kMaxLocalCount)
            CompileError::raise(
                local->location, "Out of local registers when trying to allocate %s: exceeded limit %d", local->name.value, kMaxLocalCount
            );

        localStack.push_back(local);

        Local& l = locals[local];

        LUAU_ASSERT(!l.allocated);

        l.reg = reg;
        l.allocated = true;
        l.debugpc = bytecode.getDebugPC();
        l.allocpc = allocpc == kDefaultAllocPc ? l.debugpc : allocpc;

        if (FFlag::LuauCompileMoveElision)
            regCaptured[reg] = l.captured;

        if (FFlag::LuauCompileReuseLocalRegs)
            localUses[reg]++;
    }

    bool areLocalsCaptured(size_t start)
    {
        LUAU_ASSERT(start <= localStack.size());

        for (size_t i = start; i < localStack.size(); ++i)
        {
            Local* l = locals.find(localStack[i]);
            LUAU_ASSERT(l);

            if (l->captured)
                return true;
        }

        return false;
    }

    void closeLocals(size_t start)
    {
        LUAU_ASSERT(start <= localStack.size());

        bool captured = false;
        uint8_t captureReg = 255;

        for (size_t i = start; i < localStack.size(); ++i)
        {
            Local* l = locals.find(localStack[i]);
            LUAU_ASSERT(l);

            if (l->captured)
            {
                captured = true;
                captureReg = std::min(captureReg, l->reg);
            }
        }

        if (captured)
        {
            bytecode.emitABC(LOP_CLOSEUPVALS, captureReg, 0, 0);
        }
    }

    struct Local
    {
        uint8_t reg = 0;
        bool allocated = false;
        bool captured = false;
        bool reused = false;
        uint32_t debugpc = 0;
        uint32_t allocpc = 0;
    };

    void recordLocalDebugInfo(AstLocal* astLocal, Local* l, bool isArgument, uint32_t endpc)
    {
        if (options.debugLevel >= 2)
        {
            bytecode.pushDebugLocal(sref(astLocal->name), l->reg, l->debugpc, endpc);
        }

        if (options.typeInfoLevel >= 1 && !isArgument)
        {
            LuauBytecodeType ty = LBC_TYPE_ANY;

            if (LuauBytecodeType* recordedTy = localTypes.find(astLocal))
                ty = *recordedTy;

            bytecode.pushLocalTypeInfo(ty, l->reg, l->allocpc, endpc);
        }
    }

    // Returns true if the local release has also freed the last use of the register assigned to it
    bool releaseLocal(Local* l)
    {
        LUAU_ASSERT(FFlag::LuauCompileReuseLocalRegs);

        l->allocated = false;

        if (FFlag::LuauCompileMoveElision)
            regCaptured[l->reg] = false;

        LUAU_ASSERT(localUses[l->reg] != 0);
        localUses[l->reg]--;
        return localUses[l->reg] == 0;
    }

    void popLocals(size_t start)
    {
        LUAU_ASSERT(start <= localStack.size());

        for (size_t i = start; i < localStack.size(); ++i)
        {
            Local* l = locals.find(localStack[i]);
            LUAU_ASSERT(l);

            if (FFlag::LuauCompileReuseLocalRegs)
            {
                if (l->reused)
                {
                    // Local going out of scope completely, its register can no longer be reused (array is sorted, so binary search here)
                    auto it = std::lower_bound(
                        freeLocalRegs.begin(),
                        freeLocalRegs.end(),
                        l->reg,
                        [](const FreeLocal& el, uint8_t pred)
                        {
                            return el.reg < pred;
                        }
                    );
                    if (it != freeLocalRegs.end() && it->reg == l->reg)
                    {
                        recordLocalDebugInfo(it->astLocal, &it->local, it->isArgument, bytecode.getDebugPC());
                        freeLocalRegs.erase(it);
                    }

                    l->allocated = false;
                    l->reused = false;
                }
                else if (l->allocated)
                {
                    releaseLocal(l);
                    recordLocalDebugInfo(localStack[i], l, i < argCount, bytecode.getDebugPC());
                }
            }
            else
            {
                LUAU_ASSERT(l->allocated);
                l->allocated = false;

                if (FFlag::LuauCompileMoveElision)
                    regCaptured[l->reg] = false;

                if (options.debugLevel >= 2)
                {
                    uint32_t debugpc = bytecode.getDebugPC();

                    bytecode.pushDebugLocal(sref(localStack[i]->name), l->reg, l->debugpc, debugpc);
                }

                if (options.typeInfoLevel >= 1 && i >= argCount)
                {
                    uint32_t debugpc = bytecode.getDebugPC();
                    LuauBytecodeType ty = LBC_TYPE_ANY;

                    if (LuauBytecodeType* recordedTy = localTypes.find(localStack[i]))
                        ty = *recordedTy;

                    bytecode.pushLocalTypeInfo(ty, l->reg, l->allocpc, debugpc);
                }
            }
        }

        localStack.resize(start);
    }

    void popStatLocals(AstStatBlock* owner, AstStat* beforeStat, size_t start)
    {
        if (options.optimizationLevel == 0)
            return;

        LUAU_ASSERT(FFlag::LuauCompileReuseLocalRegs);
        LUAU_ASSERT(start <= localStack.size());

        for (size_t i = start; i < localStack.size(); ++i)
        {
            Local* l = locals.find(localStack[i]);
            LUAU_ASSERT(l);

            if (!l->allocated || l->reused)
                continue;

            if (l->captured)
                continue;

            auto var = variables.find(localStack[i]);

            if (!var)
                continue;

            if (var->owner != owner || var->nonLexicalUse)
                continue;

            Position localEnd = var->lastUsed ? var->lastUsed->location.end : Position::missing();
            Position statStart = beforeStat->location.begin;

            if (!var->lastUsed || statStart.line > localEnd.line || (statStart.line == localEnd.line && statStart.column > localEnd.column))
            {
                if (releaseLocal(l))
                {
                    FreeLocal free;
                    free.reg = l->reg;
                    free.local = *l;
                    free.astLocal = localStack[i];
                    free.isArgument = i < argCount;

                    // Free registers are ordered from low to high as it is better to give out registers closer to regTop
                    auto it = std::lower_bound(
                        freeLocalRegs.begin(),
                        freeLocalRegs.end(),
                        l->reg,
                        [](const FreeLocal& el, uint8_t pred)
                        {
                            return el.reg < pred;
                        }
                    );
                    freeLocalRegs.insert(it, free);

                    l->reused = true;
                }
                else
                {
                    recordLocalDebugInfo(localStack[i], l, i < argCount, bytecode.getDebugPC());
                }
            }
        }
    }

    void patchJump(AstNode* node, size_t label, size_t target)
    {
        if (!bytecode.patchJumpD(label, target))
            CompileError::raise(node->location, "Exceeded jump distance limit; simplify the code to compile");
    }

    void patchJumps(AstNode* node, std::vector<size_t>& labels, size_t target)
    {
        for (size_t l : labels)
            patchJump(node, l, target);
    }

    void patchLoopJumps(AstNode* node, size_t oldJumps, size_t endLabel, size_t contLabel)
    {
        LUAU_ASSERT(oldJumps <= loopJumps.size());

        for (size_t i = oldJumps; i < loopJumps.size(); ++i)
        {
            const LoopJump& lj = loopJumps[i];

            switch (lj.type)
            {
            case LoopJump::Break:
                patchJump(node, lj.label, endLabel);
                break;

            case LoopJump::Continue:
                patchJump(node, lj.label, contLabel);
                break;

            default:
                LUAU_ASSERT(!"Unknown loop jump type");
            }
        }
    }

    uint8_t allocReg(AstNode* node, unsigned int count)
    {
        unsigned int top = regTop;
        if (top + count > kMaxRegisterCount)
            CompileError::raise(node->location, "Out of registers when trying to allocate %d registers: exceeded limit %d", count, kMaxRegisterCount);

        regTop += count;
        stackSize = std::max(stackSize, regTop);

        return uint8_t(top);
    }

    template<typename T>
    uint8_t allocReg(AstNode* node, T count) = delete;

    bool tryReuseLocalReg(AstLocal* local, uint8_t& reg)
    {
        LUAU_ASSERT(FFlag::LuauCompileReuseLocalRegs);

        if (local->isExported || freeLocalRegs.empty())
            return false;

        Variable* lv = variables.find(local);

        if (!lv)
            return false;

        Local* l = locals.find(local);

        if (l && l->captured)
            return false;

        FreeLocal free = freeLocalRegs.back();
        freeLocalRegs.pop_back();

        recordLocalDebugInfo(free.astLocal, &free.local, free.isArgument, bytecode.getDebugPC());
        reg = free.reg;

        LUAU_ASSERT(reg < regTop);
        return true;
    }

    void setDebugLine(AstNode* node)
    {
        if (options.debugLevel >= 1)
            bytecode.setDebugLine(node->location.begin.line + 1);
    }

    void setDebugLine(const Location& location)
    {
        if (options.debugLevel >= 1)
            bytecode.setDebugLine(location.begin.line + 1);
    }

    void setDebugLineEnd(AstNode* node)
    {
        if (options.debugLevel >= 1)
            bytecode.setDebugLine(node->location.end.line + 1);
    }

    bool needsCoverage(AstNode* node)
    {
        return !node->is<AstStatBlock>() && !node->is<AstStatTypeAlias>();
    }

    void hintTemporaryRegType(AstExpr* expr, int reg, LuauBytecodeType expectedType, int instLength)
    {
        // If we know the type of a temporary and it's not the type that would be expected by codegen, provide a hint
        if (LuauBytecodeType* ty = exprTypes.find(expr))
        {
            if (*ty != expectedType)
                bytecode.pushLocalTypeInfo(*ty, reg, bytecode.getDebugPC() - instLength, bytecode.getDebugPC());
        }
    }

    void hintTemporaryExprRegType(AstExpr* expr, int reg, LuauBytecodeType expectedType, int instLength)
    {
        // If we allocated a temporary register for the operation argument, try hinting its type
        if (!getExprLocal(expr))
            hintTemporaryRegType(expr, reg, expectedType, instLength);
    }

    struct FenvVisitor : AstVisitor
    {
        bool& getfenvUsed;
        bool& setfenvUsed;

        FenvVisitor(bool& getfenvUsed, bool& setfenvUsed)
            : getfenvUsed(getfenvUsed)
            , setfenvUsed(setfenvUsed)
        {
        }

        bool visit(AstExprGlobal* node) override
        {
            if (node->name == "getfenv")
                getfenvUsed = true;
            if (node->name == "setfenv")
                setfenvUsed = true;

            return false;
        }
    };

    struct FunctionVisitor : AstVisitor
    {
        std::vector<AstExprFunction*>& functions;
        bool hasTypes = false;
        bool hasNativeFunction = false;

        FunctionVisitor(std::vector<AstExprFunction*>& functions)
            : functions(functions)
        {
            // preallocate the result; this works around std::vector's inefficient growth policy for small arrays
            functions.reserve(16);
        }

        bool visit(AstExprFunction* node) override
        {
            node->body->visit(this);

            for (AstLocal* arg : node->args)
                hasTypes |= arg->annotation != nullptr;

            // this makes sure all functions that are used when compiling this one have been already added to the vector
            LUAU_ASSERT(functions.end() == std::find(functions.begin(), functions.end(), node));
            functions.push_back(node);

            if (!hasNativeFunction && node->hasNativeAttribute())
                hasNativeFunction = true;

            return false;
        }

        bool visit(AstStatTypeFunction* node) override
        {
            return false;
        }
    };

    struct UndefinedLocalVisitor : AstVisitor
    {
        UndefinedLocalVisitor(Compiler* self)
            : self(self)
            , undef(nullptr)
        {
        }

        void check(AstLocal* local)
        {
            if (!undef && locals.contains(local))
                undef = local;
        }

        bool visit(AstExprLocal* node) override
        {
            if (!node->upvalue)
                check(node->local);

            return false;
        }

        bool visit(AstExprFunction* node) override
        {
            const Function* f = self->functions.find(node);
            LUAU_ASSERT(f);

            for (AstLocal* uv : f->upvals)
            {
                LUAU_ASSERT(uv->functionDepth < node->functionDepth);

                if (uv->functionDepth == node->functionDepth - 1)
                    check(uv);
            }

            return false;
        }

        Compiler* self;
        AstLocal* undef;
        DenseHashSet<AstLocal*> locals;
    };

    struct ConstUpvalueVisitor : AstVisitor
    {
        ConstUpvalueVisitor(Compiler* self)
            : self(self)
        {
        }

        bool visit(AstExprLocal* node) override
        {
            if (node->upvalue && self->isConstant(node))
            {
                upvals.push_back(node->local);
            }

            return false;
        }

        bool visit(AstExprFunction* node) override
        {
            // short-circuits the traversal to make it faster
            return false;
        }

        Compiler* self;
        std::vector<AstLocal*> upvals;
    };

    enum class ReturnKind
    {
        None,       // No returns in the function
        Expression, // Function returns some expression (could be a constant)
        Parameter,  // Function returns one of its parameters
        Local,      // Function returns one of its locals

        Unknown // Function has multiple conflicting return kinds or return targets (for Parameter/Local)
    };

    struct ReturnVisitor : AstVisitor
    {
        Compiler* self = nullptr;
        AstExprFunction* func = nullptr;

        bool returnsOne = true;
        ReturnKind returnKind = ReturnKind::None;
        AstLocal* returnLocal = nullptr; // For Parameter/Local ReturnKind

        ReturnVisitor(Compiler* self, AstExprFunction* func)
            : self(self)
            , func(func)
        {
        }

        bool visit(AstExpr* expr) override
        {
            return false;
        }

        bool visit(AstStatReturn* stat) override
        {
            if (FFlag::LuauCompileMoveElision)
            {
                if (stat->list.size == 1 && !self->isExprMultRet(stat->list.data[0]))
                {
                    if (AstExprLocal* exprLocal = unwrapExprOfType<AstExprLocal>(stat->list.data[0]))
                    {
                        ReturnKind kind = ReturnKind::Local;
                        AstLocal* local = exprLocal->local;

                        for (auto el : func->args)
                        {
                            if (el == local)
                            {
                                kind = ReturnKind::Parameter;
                                break;
                            }
                        }

                        if (exprLocal->upvalue)
                        {
                            kind = ReturnKind::Expression;
                            local = nullptr;
                        }

                        if (returnKind == ReturnKind::None)
                        {
                            returnKind = kind;
                            returnLocal = local;
                        }
                        else if (returnKind != kind || returnLocal != local)
                        {
                            returnKind = ReturnKind::Unknown;
                            returnLocal = nullptr;
                        }
                    }
                    else
                    {
                        if (returnKind == ReturnKind::None)
                        {
                            returnKind = ReturnKind::Expression;
                        }
                        else if (returnKind != ReturnKind::Expression)
                        {
                            returnKind = ReturnKind::Unknown;
                            returnLocal = nullptr;
                        }
                    }
                }
                else
                {
                    returnsOne = false;
                    returnKind = ReturnKind::Unknown;
                    returnLocal = nullptr;
                }
            }
            else
            {
                returnsOne &= stat->list.size == 1 && !self->isExprMultRet(stat->list.data[0]);
            }

            return false;
        }
    };

    struct RegScope
    {
        RegScope(Compiler* self)
            : self(self)
            , oldTop(self->regTop)
        {
        }

        // This ctor is useful to forcefully adjust the stack frame in case we know that registers after a certain point are scratch and can be
        // discarded
        RegScope(Compiler* self, unsigned int top)
            : self(self)
            , oldTop(self->regTop)
        {
            LUAU_ASSERT(top <= self->regTop);
            self->regTop = top;
        }

        ~RegScope()
        {
            self->regTop = oldTop;
        }

        Compiler* self;
        unsigned int oldTop;
    };

    struct Function
    {
        uint32_t id;
        std::vector<AstLocal*> upvals;

        uint64_t costModel = 0;
        unsigned int stackSize = 0;
        bool canInline = false;
        bool returnsOne = false;

        ReturnKind returnKind = ReturnKind::None;
        AstLocal* returnLocal = nullptr; // For Parameter/Local ReturnKind
        bool returnLocalCaptured = false;
    };

    struct LoopJump
    {
        enum Type
        {
            Break,
            Continue
        };

        Type type;
        size_t label;
    };

    struct Loop
    {
        size_t localOffset;
        size_t localOffsetContinue;

        AstStatContinue* continueUsed;
    };

    struct InlineArg
    {
        AstLocal* local;

        uint8_t reg;
        Constant value;
        uint32_t allocpc;

        AstExpr* init;
    };

    struct InlineFrame
    {
        AstExprFunction* func;

        size_t localOffset;

        uint8_t target;
        uint8_t targetCount;

        AstLocal* resultLocal = nullptr;

        std::vector<size_t> returnJumps;
    };

    struct Capture
    {
        LuauCaptureType type;
        uint8_t data;
    };

    BytecodeBuilder& bytecode;

    CompileOptions options;


    DenseHashMap<AstExprFunction*, Function> functions;
    DenseHashMap<AstLocal*, Local> locals;
    DenseHashMap<AstName, Global> globals;
    DenseHashMap<AstLocal*, Variable> variables;
    DenseHashMap<AstExpr*, Constant> constants;
    DenseHashMap<AstLocal*, Constant> locstants;
    DenseHashMap<AstLocal*, TableConstantKind> tableConstants;
    DenseHashMap<AstExprTable*, TableShape> tableShapes;
    DenseHashMap<AstExprCall*, int> builtins;
    DenseHashMap<AstName, uint8_t> userdataTypes;
    DenseHashMap<AstExprFunction*, std::string> functionTypes;
    DenseHashMap<AstLocal*, LuauBytecodeType> localTypes;
    DenseHashMap<AstExpr*, LuauBytecodeType> exprTypes;

    DenseHashMap<AstExprCall*, int> inlineBuiltins;
    DenseHashMap<AstExprCall*, int> inlineBuiltinsBackup;

    Compile::ExprConstantChangeLog exprChanges;
    Compile::LocalConstantChangeLog localChanges;

    BuiltinAstTypes builtinTypes;
    AstNameTable& names;

    const DenseHashMap<AstExprCall*, int>* builtinsFold = nullptr;
    bool builtinsFoldLibraryK = false;

    // compileFunction state, gets reset for every function
    unsigned int regTop = 0;
    unsigned int stackSize = 0;
    size_t argCount = 0;
    bool hasLoops = false;
    bool hasMultiRet = false;
    AstExprFunction* currentFunction = nullptr;

    size_t blockDepth = 0;

    bool getfenvUsed = false;
    bool setfenvUsed = false;

    std::vector<AstLocal*> localStack;
    std::array<bool, kMaxRegisterCount> regCaptured{};
    std::vector<AstLocal*> upvals;
    std::vector<LoopJump> loopJumps;
    std::vector<Loop> loops;
    std::vector<InlineFrame> inlineFrames;
    std::vector<Capture> captures;

    struct FreeLocal
    {
        uint8_t reg = 0;

        Local local;
        AstLocal* astLocal = nullptr;
        bool isArgument = false;
    };
    std::array<int, kMaxRegisterCount> localUses{};
    std::vector<FreeLocal> freeLocalRegs;

    struct Exports
    {
        AstLocal exportTableLocal;
        DenseHashMap<AstLocal*, uint8_t> exportedClasses;
        DenseHashSet<AstLocal*> exportedFunctions;
        std::vector<AstLocal*> exportedVariables;
        int32_t exportedTableCid = -1;
        bool hasExports = false;
        bool hasTypeExports = false;

        explicit Exports(AstLocal tableLocal)
            : exportTableLocal(tableLocal)
        {
        }

        bool isEmpty() const
        {
            return !hasExports && exportedClasses.empty() && exportedFunctions.empty() && exportedVariables.empty();
        }
    };

    Exports exports;
};

static void setCompileOptionsForNativeCompilation(CompileOptions& options)
{
    options.optimizationLevel = 2; // note: this might be removed in the future in favor of --!optimize
    options.typeInfoLevel = 1;
}

void compileOrThrow(BytecodeBuilder& bytecode, const ParseResult& parseResult, AstNameTable& names, const CompileOptions& inputOptions)
{
    LUAU_TIMETRACE_SCOPE("compileOrThrow", "Compiler");

    LUAU_ASSERT(parseResult.root);
    LUAU_ASSERT(parseResult.errors.empty());

    CompileOptions options = inputOptions;
    uint8_t mainFlags = 0;

    for (const HotComment& hc : parseResult.hotcomments)
    {
        if (hc.header && hc.content.compare(0, 9, "optimize ") == 0)
            options.optimizationLevel = std::max(0, std::min(2, atoi(hc.content.c_str() + 9)));

        if (hc.header && hc.content == "native")
        {
            mainFlags |= LPF_NATIVE_MODULE;
            setCompileOptionsForNativeCompilation(options);
        }
    }

    AstStatBlock* root = parseResult.root;

    // gathers all functions with the invariant that all function references are to functions earlier in the list
    // for example, function foo() return function() end end will result in two vector entries, [0] = anonymous and [1] = foo
    std::vector<AstExprFunction*> functions;
    Compiler::FunctionVisitor functionVisitor(functions);
    root->visit(&functionVisitor);

    if (functionVisitor.hasNativeFunction)
        setCompileOptionsForNativeCompilation(options);

    Compiler compiler(bytecode, options, names);

    // since access to some global objects may result in values that change over time, we block imports from non-readonly tables
    assignMutable(compiler.globals, names, options.mutableGlobals);

    // this pass analyzes mutability of locals/globals and associates locals with their initial values
    if (FFlag::LuauOptimizeExportTable)
        trackValues(compiler.globals, compiler.variables, compiler.exports.exportedFunctions, compiler.exports.exportedVariables, root);
    else
        trackValues_DEPRECATED(compiler.globals, compiler.variables, root);

    // this visitor tracks calls to getfenv/setfenv and disables some optimizations when they are found
    if (options.optimizationLevel >= 1 && (names.get("getfenv").value || names.get("setfenv").value))
    {
        Compiler::FenvVisitor fenvVisitor(compiler.getfenvUsed, compiler.setfenvUsed);
        root->visit(&fenvVisitor);
    }

    // builtin folding is enabled on optimization level 2 since we can't de-optimize folding at runtime
    if (options.optimizationLevel >= 2 && (!compiler.getfenvUsed && !compiler.setfenvUsed))
    {
        compiler.builtinsFold = &compiler.builtins;

        if (AstName math = names.get("math"); math.value && getGlobalState(compiler.globals, math) == Global::Default)
        {
            compiler.builtinsFoldLibraryK = true;
        }
        else if (const char* const* ptr = options.librariesWithKnownMembers)
        {
            for (; *ptr; ++ptr)
            {
                if (AstName name = names.get(*ptr); name.value && getGlobalState(compiler.globals, name) == Global::Default)
                {
                    compiler.builtinsFoldLibraryK = true;
                    break;
                }
            }
        }
    }

    if (options.optimizationLevel >= 1)
    {
        // this pass tracks which calls are builtins and can be compiled more efficiently
        analyzeBuiltins(compiler.builtins, compiler.globals, compiler.variables, options, root, names);

        // this pass determines which locals hold constant tables that are never mutated
        buildTableConstantMap(compiler.tableConstants, compiler.variables, root);

        // this pass analyzes constantness of expressions
        foldConstants(
            compiler.constants,
            compiler.variables,
            compiler.locstants,
            compiler.builtinsFold,
            compiler.builtinsFoldLibraryK,
            options.vectorPrecision == 1,
            options.libraryMemberConstantCb,
            root,
            names,
            compiler.tableConstants
        );

        // this pass analyzes table assignments to estimate table shapes for initially empty tables
        predictTableShapes(compiler.tableShapes, root);
    }

    if (const char* const* ptr = options.userdataTypes)
    {
        for (; *ptr; ++ptr)
        {
            // Type will only resolve to an AstName if it is actually mentioned in the source
            if (AstName name = names.get(*ptr); name.value)
                compiler.userdataTypes[name] = bytecode.addUserdataType(name.value);
        }

        if (uintptr_t(ptr - options.userdataTypes) > (LBC_TYPE_TAGGED_USERDATA_END - LBC_TYPE_TAGGED_USERDATA_BASE))
            CompileError::raise(root->location, "Exceeded userdata type limit in the compilation options");
    }

    // computes type information for all functions based on type annotations
    if (options.typeInfoLevel >= 1 || options.optimizationLevel >= 2)
        buildTypeMap(
            compiler.functionTypes,
            compiler.localTypes,
            compiler.exprTypes,
            root,
            options.vectorType,
            compiler.userdataTypes,
            compiler.builtinTypes,
            compiler.builtins,
            compiler.globals,
            options.libraryMemberTypeCb,
            bytecode
        );

    for (AstExprFunction* expr : functions)
    {
        uint8_t protoflags = 0;
        compiler.compileFunction(expr, protoflags);

        // If a function has native attribute and the whole module is not native, we set  LPF_NATIVE_FUNCTION flag
        // This ensures that LPF_NATIVE_MODULE and LPF_NATIVE_FUNCTION are exclusive.
        if ((protoflags & LPF_NATIVE_FUNCTION) && !(mainFlags & LPF_NATIVE_MODULE))
            mainFlags |= LPF_NATIVE_FUNCTION;
    }

    AstExprFunction main(
        root->location,
        /* attributes= */ AstArray<AstAttr*>({nullptr, 0}),
        /* generics= */ AstArray<AstGenericType*>(),
        /* genericPacks= */ AstArray<AstGenericTypePack*>(),
        /* self= */ nullptr,
        AstArray<AstLocal*>(),
        /* vararg= */ true,
        /* varargLocation= */ Luau::Location(),
        root,
        /* functionDepth= */ 0,
        /* debugname= */ AstName(),
        /* returnAnnotation= */ nullptr
    );
    uint32_t mainid = compiler.compileFunction(&main, mainFlags);

    const Compiler::Function* mainf = compiler.functions.find(&main);
    LUAU_ASSERT(mainf && mainf->upvals.empty());

    bytecode.setMainFunction(mainid);
    bytecode.finalize();
}

void compileOrThrow(BytecodeBuilder& bytecode, const std::string& source, const CompileOptions& options, const ParseOptions& parseOptions)
{
    Allocator allocator;
    AstNameTable names(allocator);
    ParseResult result = Parser::parse(source.c_str(), source.size(), names, allocator, parseOptions);

    if (!result.errors.empty())
        throw ParseErrors(result.errors);

    compileOrThrow(bytecode, result, names, options);
}

std::string compile(const std::string& source, const CompileOptions& options, const ParseOptions& parseOptions, BytecodeEncoder* encoder)
{
    LUAU_TIMETRACE_SCOPE("compile", "Compiler");

    Allocator allocator;
    AstNameTable names(allocator);
    ParseResult result = Parser::parse(source.c_str(), source.size(), names, allocator, parseOptions);

    if (!result.errors.empty())
    {
        // Users of this function expect only a single error message
        const Luau::ParseError& parseError = result.errors.front();
        std::string error = format(":%d: %s", parseError.getLocation().begin.line + 1, parseError.what());

        return BytecodeBuilder::getError(error);
    }

    try
    {
        BytecodeBuilder bcb(encoder);
        compileOrThrow(bcb, result, names, options);

        return bcb.getBytecode();
    }
    catch (CompileError& e)
    {
        std::string error = format(":%d: %s", e.getLocation().begin.line + 1, e.what());
        return BytecodeBuilder::getError(error);
    }
}

void setCompileConstantNil(CompileConstant* constant)
{
    Compile::Constant* target = reinterpret_cast<Compile::Constant*>(constant);

    target->type = Compile::Constant::Type_Nil;
}

void setCompileConstantBoolean(CompileConstant* constant, bool b)
{
    Compile::Constant* target = reinterpret_cast<Compile::Constant*>(constant);

    target->type = Compile::Constant::Type_Boolean;
    target->valueBoolean = b;
}

void setCompileConstantNumber(CompileConstant* constant, double n)
{
    Compile::Constant* target = reinterpret_cast<Compile::Constant*>(constant);

    target->type = Compile::Constant::Type_Number;
    target->valueNumber = n;
}

void setCompileConstantInteger64(CompileConstant* constant, int64_t l)
{
    Compile::Constant* target = reinterpret_cast<Compile::Constant*>(constant);

    target->type = Compile::Constant::Type_Integer;
    target->valueInteger64 = l;
}

void setCompileConstantVector(CompileConstant* constant, float x, float y, float z, float w)
{
    Compile::Constant* target = reinterpret_cast<Compile::Constant*>(constant);

    target->type = Compile::Constant::Type_Vectorf;
    target->valueVectorf[0] = x;
    target->valueVectorf[1] = y;
    target->valueVectorf[2] = z;
    target->valueVectorf[3] = w;
}

void setCompileConstantVectord(CompileConstant* constant, double x, double y, double z, double w)
{
    Compile::Constant* target = reinterpret_cast<Compile::Constant*>(constant);

    target->type = Compile::Constant::Type_Vectord;
    target->valueVectord[0] = x;
    target->valueVectord[1] = y;
    target->valueVectord[2] = z;
    target->valueVectord[3] = w;
}

void setCompileConstantString(CompileConstant* constant, const char* s, size_t l)
{
    Compile::Constant* target = reinterpret_cast<Compile::Constant*>(constant);

    if (l > std::numeric_limits<unsigned int>::max())
        CompileError::raise({}, "Exceeded custom string constant length limit");

    target->type = Compile::Constant::Type_String;
    target->stringLength = unsigned(l);
    target->valueString = s;
}

} // namespace Luau
