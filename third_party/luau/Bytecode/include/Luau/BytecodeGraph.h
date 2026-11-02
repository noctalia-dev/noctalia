// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/Bytecode.h"
#include "Luau/BytecodeBuilder.h"
#include "Luau/DenseHash.h"
#include "Luau/SmallVector.h"

#include <algorithm>
#include <list>
#include <optional>
#include <vector>
#include <unordered_map>

#include <stdint.h>
#include <string.h>

namespace Luau
{
namespace Bytecode
{

using Instruction = uint32_t;
using Reg = uint8_t;

enum class BcOpKind : uint32_t
{
    None,

    // To reference a immediate value
    Imm,

    // To reference a result of a previous instruction
    Inst,

    // To reference a basic block in control flow
    Block,

    // Phi operand
    Phi,

    // Projection of multireturn call or variadic arguments
    Proj,

    // To reference a VM register
    VmReg,

    // To reference a VM constant
    VmConst,

    // To reference a VM upvalue
    VmUpvalue,

    // To reference a VM upvalue
    VmProto,
};

struct BcOp
{
    BcOpKind kind : 4;
    uint32_t index : 28;

    BcOp()
        : kind(BcOpKind::None)
        , index(0)
    {
    }

    BcOp(BcOpKind kind, uint32_t index)
        : kind(kind)
        , index(index)
    {
    }

    bool operator==(const BcOp& rhs) const
    {
        return kind == rhs.kind && index == rhs.index;
    }

    bool operator!=(const BcOp& rhs) const
    {
        return !(*this == rhs);
    }
};

static_assert(sizeof(BcOp) == 4);

struct BcOpHash
{
    size_t operator()(const BcOp& p) const
    {
        size_t res = 0;
        memcpy(&res, &p, sizeof(p));
        return res;
    }
};

using RegMap = std::unordered_map<BcOp, Reg, BcOpHash>;

enum class BcImmKind : uint8_t
{
    Boolean,
    Int,
    Import
};

struct BcImm
{
    BcImmKind kind;

    union
    {
        bool valueBoolean;
        int32_t valueInt;
        uint32_t valueImport;
    };

    bool operator==(const BcImm& rhs) const
    {
        if (kind == BcImmKind::Boolean && rhs.kind == BcImmKind::Boolean)
            return valueBoolean == rhs.valueBoolean;
        else if (kind == BcImmKind::Int && rhs.kind == BcImmKind::Int)
            return valueInt == rhs.valueInt;
        else if (kind == BcImmKind::Import && rhs.kind == BcImmKind::Import)
            return valueImport == rhs.valueImport;
        else
            return false;
    }

    bool operator!=(const BcImm& rhs) const
    {
        return !(*this == rhs);
    }
};

enum class BcVmConstKind : uint8_t
{
    Nil,
    Boolean,
    Number,
    Vectorf,
    Vectord,
    String,
    Import,
    Table,
    Closure,
    Integer,
    ClassShape
};

struct BcVmConst
{
    BcVmConstKind kind;

    union
    {
        bool valueBoolean;
        double valueNumber;
        float valueVectorf[4];
        double valueVectord[4];
        std::string_view valueString;
        uint32_t valueImport;
        uint32_t valueTable;
        uint32_t valueClosure;
        int64_t valueInteger;
        uint32_t valueClassShape;
    };

    BcVmConst()
        : kind(BcVmConstKind::Nil)
        , valueBoolean(0)
    {
    }

    bool operator==(const BcVmConst& rhs) const
    {
        if (kind != rhs.kind)
            return false;

        switch (kind)
        {
        case BcVmConstKind::Nil:
            return true;

        case BcVmConstKind::Boolean:
            return valueBoolean == rhs.valueBoolean;

        case BcVmConstKind::Number:
            return valueNumber == rhs.valueNumber;

        case BcVmConstKind::Vectorf:
            return valueVectorf[0] == rhs.valueVectorf[0] && valueVectorf[1] == rhs.valueVectorf[1] && valueVectorf[2] == rhs.valueVectorf[2] &&
                   valueVectorf[3] == rhs.valueVectorf[3];

        case BcVmConstKind::Vectord:
            return valueVectord[0] == rhs.valueVectord[0] && valueVectord[1] == rhs.valueVectord[1] && valueVectord[2] == rhs.valueVectord[2] &&
                   valueVectord[3] == rhs.valueVectord[3];

        case BcVmConstKind::String:
            return valueString == rhs.valueString;

        case BcVmConstKind::Import:
            return valueImport == rhs.valueImport;

        case BcVmConstKind::Table:
            return valueTable == rhs.valueTable;

        case BcVmConstKind::Closure:
            return valueClosure == rhs.valueClosure;

        case BcVmConstKind::Integer:
            return valueInteger == rhs.valueInteger;

        case BcVmConstKind::ClassShape:
            return valueClassShape == rhs.valueClassShape;

        default:
            LUAU_ASSERT(!"Unhandled BcVmConstKind");
            return false;
        }
        return false;
    }

    bool operator!=(const BcVmConst& rhs) const
    {
        return !(*this == rhs);
    }
};

using BcOps = SmallVector<BcOp, 4>;

struct BcInst
{
    LuauOpcode op;
    BcOp block;

    // Operands
    BcOps ops;
    std::vector<BcOp> uses;

    uint32_t lastUse = 0;
    uint32_t useCount = 0;
    uint32_t line = 0;
};

// When IrInst operands are used, current instruction index is often required to track lifetime
inline constexpr uint32_t kInvalidInstIdx = ~0u;

struct BcInstHash
{
    static const uint32_t m = 0x5bd1e995;
    static const int r = 24;

    static uint32_t mix(uint32_t h, uint32_t k)
    {
        // MurmurHash2 step
        k *= m;
        k ^= k >> r;
        k *= m;

        h *= m;
        h ^= k;

        return h;
    }

    static uint32_t mix(uint32_t h, BcOp op)
    {
        static_assert(sizeof(op) == sizeof(uint32_t));
        uint32_t k;
        memcpy(&k, &op, sizeof(op));

        return mix(h, k);
    }

    size_t operator()(const BcInst& key) const
    {
        // MurmurHash2 unrolled
        uint32_t h = 25;

        h = mix(h, uint32_t(key.op));
        for (size_t i = 0; i < 7; i++)
            h = mix(h, i < uint32_t(key.ops.size()) ? key.ops[i] : BcOp{});

        // MurmurHash2 tail
        h ^= h >> 13;
        h *= m;
        h ^= h >> 15;

        return h;
    }
};

struct BcInstEq
{
    bool operator()(const BcInst& a, const BcInst& b) const
    {
        if (a.op != b.op || a.ops.size() != b.ops.size())
            return false;
        for (size_t i = 0; i < a.ops.size(); i++)
            if (a.ops[i] != b.ops[i])
                return false;
        return true;
    }
};

inline constexpr uint32_t kBlockNoStartPc = ~0u;

enum BcBlockEdgeKind
{
    Branch,
    Fallthrough,
    Loop
};

struct BcBlockEdge
{
    BcBlockEdgeKind kind;
    BcOp target;
};

using BcEdges = SmallVector<BcBlockEdge, 2>;

enum BcBlockFlag
{
    Dead = 1 << 0
};

struct BcBlock
{
    uint8_t flags = 0;
    uint32_t useCount = 0;

    std::list<BcOp> phis;
    std::list<BcOp> ops;
    BcEdges successors;
    BcEdges predecessors;

    uint32_t sortkey = ~0u;
    uint32_t chainkey = 0;

    // Bytecode PC position at which the block was generated
    uint32_t startpc = kBlockNoStartPc;

    void appendInstruction(BcOp inst)
    {
        LUAU_ASSERT(inst.kind == BcOpKind::Inst);
        ops.push_back(inst);
    }
};

struct BcPhi
{
    BcOps ops;
    std::vector<BcOp> uses;
};

struct BcProj
{
    BcOp op;
    uint32_t index;
};

enum class BcCondition : uint8_t
{
    Equal,
    LessEqual,
    Less,
    NotEqual,
    NotLessEqual,
    NotLess,
};

inline BcCondition opcodeToCondition(LuauOpcode op)
{
    switch (op)
    {
    case LOP_JUMPIFEQ:
        return BcCondition::Equal;
    case LOP_JUMPIFLE:
        return BcCondition::LessEqual;
    case LOP_JUMPIFLT:
        return BcCondition::Less;
    case LOP_JUMPIFNOTEQ:
        return BcCondition::NotEqual;
    case LOP_JUMPIFNOTLE:
        return BcCondition::NotLessEqual;
    case LOP_JUMPIFNOTLT:
        return BcCondition::NotLess;
    default:
        LUAU_ASSERT(!"cannot map this opcode to condition");
        return BcCondition::Equal;
    }
}

struct BcFeedbackSlot
{
    LuauFeedbackType kind;

    union
    {
        struct
        {
            uint32_t inst;
        } callTarget;

        struct
        {
            uint32_t shape;
        } construct;
    };
};

struct TypedLocal
{
    LuauBytecodeType type;
    uint8_t reg;
    uint32_t startpc;
    uint32_t endpc;
};

struct DebugLocal
{
    std::string_view varname;
    uint8_t reg;
    uint32_t startpc;
    uint32_t endpc;
};

template<typename T>
struct BcRef
{
    std::vector<T>& vec;
    BcOp op;

    T* operator->()
    {
        LUAU_ASSERT(op.index < vec.size());
        return &vec[op.index];
    }

    T& operator*()
    {
        LUAU_ASSERT(op.index < vec.size());
        return vec[op.index];
    }
};

template<typename VmConst>
struct BcFunction
{
    uint8_t maxstacksize = 0;
    uint8_t numparams = 0;
    uint8_t nups = 0;
    bool is_vararg = false;
    uint8_t flags = 0;

    std::vector<BcBlock> blocks;
    std::vector<BcInst> instructions;
    std::vector<VmConst> constants;
    std::vector<BcImm> immediates;
    std::vector<BcPhi> phis;
    std::vector<BcProj> projections;
    std::vector<BytecodeBuilder::TableShape> tableShapes;
    std::vector<BytecodeBuilder::ClassShape> classShapes;
    std::vector<BcFeedbackSlot> feedbackSlots;

    BcOp entryBlock;
    BcOp exitBlock;

    std::string typeInfo;
    std::vector<LuauBytecodeType> upvalueTypes;
    std::vector<TypedLocal> localTypes;
    std::vector<uint32_t> protos;

    std::string debugname;
    uint32_t linedefined = ~0u;
    std::vector<std::string_view> upvalueNames;
    std::vector<DebugLocal> locals;

    RegMap regs;

    BcOp addBlock()
    {
        blocks.emplace_back(BcBlock{});
        return BcOp{BcOpKind::Block, static_cast<uint32_t>(blocks.size() - 1)};
    }

    void addEdge(BcOp from, BcOp to, BcBlockEdgeKind kind)
    {
        blockOp(from).successors.push_back({kind, to});
        blockOp(to).predecessors.push_back({kind, from});
    }

    BcOp addInst()
    {
        instructions.emplace_back(BcInst{});
        return BcOp{BcOpKind::Inst, static_cast<uint32_t>(instructions.size() - 1)};
    }

    BcOp addInst(LuauOpcode op, BcOp block, std::initializer_list<BcOp> operands = {}, Reg outReg = kInvalidReg)
    {
        BcOp instOp = addInst();
        BcRef<BcInst> inst = this->inst(instOp);
        inst->op = op;
        inst->block = block;
        blockOp(block).appendInstruction(instOp);

        for (BcOp operand : operands)
            addUse(inst, operand);

        if (outReg != kInvalidReg)
            regs[instOp] = outReg;

        return instOp;
    }

    BcOp addPhi()
    {
        phis.emplace_back(BcPhi{});
        return BcOp{BcOpKind::Phi, static_cast<uint32_t>(phis.size() - 1)};
    }

    BcOp addPhi(BcOp block, std::initializer_list<BcOp> operands, Reg reg)
    {
        BcOp phiOp = addPhi();
        BcRef<BcPhi> phi = this->phi(phiOp);
        blockOp(block).phis.push_back(phiOp);
        regs[phiOp] = reg;

        for (BcOp operand : operands)
            addUse(phi, operand);

        return phiOp;
    }

    BcOp addProj(BcOp op, uint32_t index)
    {
        projections.emplace_back(BcProj{op, index});
        return BcOp{BcOpKind::Proj, static_cast<uint32_t>(projections.size() - 1)};
    }

    BcBlock& blockOp(BcOp op)
    {
        LUAU_ASSERT(op.kind == BcOpKind::Block);
        return blocks[op.index];
    }

    BcInst& instOp(BcOp op)
    {
        LUAU_ASSERT(op.kind == BcOpKind::Inst);
        return instructions[op.index];
    }

    BcInst* asInstOp(BcOp op)
    {
        if (op.kind == BcOpKind::Inst)
            return &instructions[op.index];

        return nullptr;
    }

    BcImm& immOp(BcOp op)
    {
        LUAU_ASSERT(op.kind == BcOpKind::Imm);
        return immediates[op.index];
    }

    VmConst& constOp(BcOp op)
    {
        LUAU_ASSERT(op.kind == BcOpKind::VmConst);
        return constants[op.index];
    }

    BcPhi& phiOp(BcOp op)
    {
        LUAU_ASSERT(op.kind == BcOpKind::Phi);
        return phis[op.index];
    }

    BcProj& projOp(BcOp op)
    {
        LUAU_ASSERT(op.kind == BcOpKind::Proj);
        return projections[op.index];
    }

    uint32_t getBlockIndex(const BcBlock& block) const
    {
        // Can only be called with blocks from our vector
        LUAU_ASSERT(&block >= blocks.data() && &block <= blocks.data() + blocks.size());
        return uint32_t(&block - blocks.data());
    }

    uint32_t getInstIndex(const BcInst& inst) const
    {
        // Can only be called with instructions from our vector
        LUAU_ASSERT(&inst >= instructions.data() && &inst <= instructions.data() + instructions.size());
        return uint32_t(&inst - instructions.data());
    }

    BcOp addImm(BcImmKind kind)
    {
        BcImm imm{kind};
        imm.valueInt = 0;
        immediates.emplace_back(imm);
        return BcOp{BcOpKind::Imm, static_cast<uint32_t>(immediates.size() - 1)};
    }

    BcOp addImm(const BcImm& imm)
    {
        immediates.emplace_back(imm);
        return BcOp{BcOpKind::Imm, static_cast<uint32_t>(immediates.size() - 1)};
    }

    BcOp addImmBool(bool value)
    {
        BcImm imm{BcImmKind::Boolean};
        imm.valueBoolean = value;
        return addImm(imm);
    }

    BcOp addImmInt(int32_t value)
    {
        BcImm imm{BcImmKind::Int};
        imm.valueInt = value;
        return addImm(imm);
    }

    BcOp addConst(const VmConst& value)
    {
        constants.emplace_back(value);
        return BcOp{BcOpKind::VmConst, static_cast<uint32_t>(constants.size() - 1)};
    }

    BcOp addVmReg(Reg reg)
    {
        return BcOp{BcOpKind::VmReg, reg};
    }

    BcOp addVmUpvalue(uint32_t upvalue)
    {
        return BcOp{BcOpKind::VmUpvalue, upvalue};
    }

    BcRef<BcBlock> block(BcOp op)
    {
        LUAU_ASSERT(op.kind == BcOpKind::Block);
        return {blocks, op};
    }

    BcRef<BcInst> inst(BcOp op)
    {
        LUAU_ASSERT(op.kind == BcOpKind::Inst);
        return {instructions, op};
    }

    template<typename T>
    T as(BcOp op)
    {
        BcRef<BcInst> insn = inst(op);
        LUAU_ASSERT(insn->op == T::opcode);

        return T{*this, insn};
    }

    BcRef<BcImm> imm(BcOp op)
    {
        LUAU_ASSERT(op.kind == BcOpKind::Imm);
        return {immediates, op};
    }

    BcRef<BcPhi> phi(BcOp op)
    {
        LUAU_ASSERT(op.kind == BcOpKind::Phi);
        return {phis, op};
    }

    BcRef<BcProj> proj(BcOp op)
    {
        LUAU_ASSERT(op.kind == BcOpKind::Proj);
        return {projections, op};
    }

    BcRef<VmConst> vmConst(BcOp op)
    {
        LUAU_ASSERT(op.kind == BcOpKind::VmConst);
        return {constants, op};
    }

    void recordUse(BcOp usedOp, BcOp user)
    {
        if (usedOp.kind == BcOpKind::Inst)
            this->instOp(usedOp).uses.push_back(user);
        else if (usedOp.kind == BcOpKind::Phi)
            this->phiOp(usedOp).uses.push_back(user);
    }

    void addUse(BcRef<BcInst> instUser, BcOp usedOp)
    {
        instUser->ops.push_back(usedOp);
        recordUse(usedOp, instUser.op);
    }

    void addUse(BcRef<BcPhi> phiUser, BcOp usedOp)
    {
        phiUser->ops.push_back(usedOp);
        recordUse(usedOp, phiUser.op);
    }

    void eraseUse(BcOp userOp, BcOp usedOp)
    {
        if (usedOp.kind == BcOpKind::Inst)
        {
            BcRef<BcInst> usedInst = inst(usedOp);
            usedInst->uses.erase(std::remove(usedInst->uses.begin(), usedInst->uses.end(), userOp), usedInst->uses.end());
        }
        else if (usedOp.kind == BcOpKind::Phi)
        {
            BcRef<BcPhi> usedPhi = phi(usedOp);
            usedPhi->uses.erase(std::remove(usedPhi->uses.begin(), usedPhi->uses.end(), userOp), usedPhi->uses.end());
        }
    }

    void eraseOp(BcOp op)
    {
        BcRef<BcInst> instRef = inst(op);

        for (BcOp usedOp : instRef->ops)
            eraseUse(op, usedOp);
        instRef->ops.clear();

        BcRef<BcBlock> blockRef = block(instRef->block);
        blockRef->ops.erase(std::remove(blockRef->ops.begin(), blockRef->ops.end(), op), blockRef->ops.end());
    }

    // replace the instruction's operands while keeping def->use links consistent
    void setOps(BcOp op, BcRef<BcInst> inst, std::initializer_list<BcOp> newOps)
    {
        for (BcOp oldOp : inst->ops)
            eraseUse(op, oldOp);
        inst->ops.clear();
        for (BcOp newOp : newOps)
        {
            inst->ops.push_back(newOp);
            recordUse(newOp, op);
        }
    };
};

using CompTimeBcFunction = BcFunction<BcVmConst>;

inline BcOp addVmConstNumber(CompTimeBcFunction& func, double value)
{
    BcVmConst constant;
    constant.kind = BcVmConstKind::Number;
    constant.valueNumber = value;
    return func.addConst(constant);
}

std::optional<CompTimeBcFunction> fromFunctionBytecode(std::string bytecode, std::vector<std::string_view>& strings);
std::string toFunctionBytecode(CompTimeBcFunction& fn);
std::string toFunctionBytecode(BytecodeBuilder& bcb, CompTimeBcFunction& fn);

} // namespace Bytecode
} // namespace Luau
