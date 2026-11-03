// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/BytecodeGraph.h"

#include <algorithm>

namespace Luau
{
namespace Bytecode
{

template<typename VmConst, typename T>
struct BcInstHelper
{
    BcFunction<VmConst>& graph;
    BcRef<BcInst> inst;

    static T create(BcFunction<VmConst>& graph)
    {
        BcOp op = graph.addInst();
        BcRef<BcInst> inst = graph.inst(op);
        inst->op = T::opcode;
        return {graph, inst};
    }

    static T from(BcFunction<VmConst>& graph, BcRef<BcInst>& inst)
    {
        LUAU_ASSERT(inst->op == T::opcode);
        return {graph, inst};
    }

    BcInst* operator->()
    {
        return &inst.vec[inst.op.index];
    }

    BcOp op()
    {
        return inst.op;
    }

    void prependTo(BcOp block)
    {
        inst->block = block;
        graph.blockOp(block).ops.push_front(inst.op);
    }

    void appendTo(BcOp block)
    {
        inst->block = block;
        graph.blockOp(block).appendInstruction(inst.op);
    }

    void insertBefore(BcRef<BcInst> op)
    {
        inst->block = op->block;
        BcRef<BcBlock> block = graph.block(op->block);
        auto it = std::find(block->ops.begin(), block->ops.end(), op.op);
        LUAU_ASSERT(it != block->ops.end());
        block->ops.insert(it, inst.op);
    }

    void detach()
    {
        if (inst->block.kind != BcOpKind::Block)
            return;
        BcRef<BcBlock> block = graph.block(inst->block);
        block->ops.remove(inst.op);
        inst->block = BcOp{};
    }

    void setOutReg(Reg out)
    {
        graph.regs[inst.op] = out;
    }

    Reg getOutReg()
    {
        auto it = graph.regs.find(inst.op);
        LUAU_ASSERT(it != graph.regs.end());
        return it->second;
    }

protected:
    int intImmInput(uint32_t inputIdx)
    {
        LUAU_ASSERT(inputIdx < inst->ops.size());
        BcImm& imm = graph.immOp(inst->ops[inputIdx]);
        return imm.valueInt;
    }

    void setImmInput(uint32_t inputIdx, int value)
    {
        if (getBcOp(inputIdx).kind == BcOpKind::None)
            inst->ops[inputIdx] = graph.addImm(BcImmKind::Int);
        BcImm& imm = graph.immOp(inst->ops[inputIdx]);
        LUAU_ASSERT(imm.kind == BcImmKind::Int);
        imm.valueInt = value;
    }

    BcOp getBcOp(uint32_t inputIdx)
    {
        if (inputIdx >= inst->ops.size())
            inst->ops.resize(inputIdx + 1);
        return inst->ops[inputIdx];
    }

    void setBcOp(uint32_t inputIdx, BcOp op)
    {
        if (inputIdx >= inst->ops.size())
            inst->ops.resize(inputIdx + 1);
        inst->ops[inputIdx] = op;

        if (op.kind == BcOpKind::Inst)
        {
            BcRef<BcInst> opInst = graph.inst(op);
            if (std::find(opInst->uses.begin(), opInst->uses.end(), inst.op) == opInst->uses.end())
            {
                opInst->uses.push_back(inst.op);
            }
        }
        else if (op.kind == BcOpKind::Phi)
        {
            BcRef<BcPhi> opPhi = graph.phi(op);
            if (std::find(opPhi->uses.begin(), opPhi->uses.end(), inst.op) == opPhi->uses.end())
            {
                opPhi->uses.push_back(inst.op);
            }
        }
    }

    BcRef<VmConst> getVmConst(uint32_t inputIdx)
    {
        BcOp constOp = getBcOp(inputIdx);
        LUAU_ASSERT(constOp.kind == BcOpKind::VmConst);
        return graph.vmConst(constOp);
    }

    void setVmConst(uint32_t inputIdx, uint32_t cid)
    {
        LUAU_ASSERT(cid < graph.constants.size());
        setBcOp(inputIdx, BcOp{BcOpKind::VmConst, cid});
    }

    BcRef<BcBlock> getBlock(uint32_t inputIdx)
    {
        BcOp blockOp = getBcOp(inputIdx);
        LUAU_ASSERT(blockOp.kind == BcOpKind::Block);
        return graph.block(blockOp);
    }

    std::vector<BcOp> sliceInputs(uint32_t startFrom)
    {
        BcOps& ops = this->inst->ops;
        std::vector<BcOp> result;
        result.reserve(ops.size() - startFrom);
        for (uint32_t i = startFrom; i < ops.size(); i++)
            result.push_back(ops[i]);
        return result;
    }
};

#define INT_IMM(name, idx) \
    static const uint32_t k##name = idx; \
    int name() \
    { \
        return this->intImmInput(idx); \
    } \
    void set##name(int value) \
    { \
        this->setImmInput(idx, value); \
    }

#define BC_OP(name, idx) \
    static const uint32_t k##name = idx; \
    BcOp name() \
    { \
        return this->getBcOp(idx); \
    } \
    void set##name(BcOp value) \
    { \
        this->setBcOp(idx, value); \
    }

#define VM_CONST(name, idx) \
    static const uint32_t k##name = idx; \
    BcRef<VmConst> name() \
    { \
        return this->getVmConst(idx); \
    } \
    void set##name(uint32_t cid) \
    { \
        this->setVmConst(idx, cid); \
    }

#define JUMP_TO(name, idx) \
    static const uint32_t k##name = idx; \
    BcRef<BcBlock> name() \
    { \
        return this->getBlock(idx); \
    } \
    void set##name(BcOp block) \
    { \
        this->setBcOp(idx, block); \
    }

template<typename VmConst = BcVmConst>
struct BcMove : public BcInstHelper<VmConst, BcMove<VmConst>>
{
    static const LuauOpcode opcode = LOP_MOVE;
    BC_OP(Src, 0)
};

template<typename VmConst = BcVmConst>
struct BcCall : public BcInstHelper<VmConst, BcCall<VmConst>>
{
    static const LuauOpcode opcode = LOP_CALL;
    INT_IMM(ParamCount, 0)
    INT_IMM(ReturnCount, 1)
    BC_OP(Target, 2)

    static const uint32_t kParamStartInput = 3;
    std::vector<BcOp> params()
    {
        return this->sliceInputs(kParamStartInput);
    }
};

template<typename VmConst = BcVmConst>
struct BcCallFB : public BcInstHelper<VmConst, BcCallFB<VmConst>>
{
    static const LuauOpcode opcode = LOP_CALLFB;
    INT_IMM(ParamCount, 0)
    INT_IMM(ReturnCount, 1)
    INT_IMM(FbSlot, 2)
    BC_OP(Target, 3)

    static const uint32_t kParamStartInput = 4;
    std::vector<BcOp> params()
    {
        return this->sliceInputs(kParamStartInput);
    }
};

template<typename VmConst = BcVmConst>
struct BcReturn : public BcInstHelper<VmConst, BcReturn<VmConst>>
{
    static const LuauOpcode opcode = LOP_RETURN;
    INT_IMM(ReturnCount, 0)

    static const uint32_t kValuesStartInput = 1;
    std::vector<BcOp> values()
    {
        if (ReturnCount() == 0)
            return {};
        return this->sliceInputs(kValuesStartInput);
    }
};

template<typename VmConst = BcVmConst>
struct BcLoadNil : public BcInstHelper<VmConst, BcLoadNil<VmConst>>
{
    static const LuauOpcode opcode = LOP_LOADNIL;
};

template<typename VmConst = BcVmConst>
struct BcNamecall : public BcInstHelper<VmConst, BcNamecall<VmConst>>
{
    static const LuauOpcode opcode = LOP_NAMECALL;
    BC_OP(Table, 0)
    INT_IMM(Hint, 1)
    VM_CONST(Key, 2)
};

template<typename VmConst = BcVmConst>
struct BcGetTableKS : public BcInstHelper<VmConst, BcGetTableKS<VmConst>>
{
    static const LuauOpcode opcode = LOP_GETTABLEKS;
    BC_OP(Source, 0)
    INT_IMM(Hint, 1)
    VM_CONST(Key, 2)
};

template<typename VmConst = BcVmConst>
struct BcGetVarArgs : public BcInstHelper<VmConst, BcGetVarArgs<VmConst>>
{
    static const LuauOpcode opcode = LOP_GETVARARGS;
    static const uint32_t kStartRegInput = 0;
    INT_IMM(ValuesCount, 1)

    Reg startReg()
    {
        return this->inst->ops[kStartRegInput].index;
    }
};

template<typename VmConst = BcVmConst>
struct BcJump : public BcInstHelper<VmConst, BcJump<VmConst>>
{
    static const LuauOpcode opcode = LOP_JUMP;
    JUMP_TO(Target, 0)
};

template<typename VmConst = BcVmConst>
struct BcCmpProto : public BcInstHelper<VmConst, BcCmpProto<VmConst>>
{
    static const LuauOpcode opcode = LOP_CMPPROTO;

    BC_OP(Closure, 0)
    INT_IMM(ProtoId, 1)
    JUMP_TO(Fallback, 2)
};

template<typename VmConst = BcVmConst>
struct BcSetList : public BcInstHelper<VmConst, BcSetList<VmConst>>
{
    static const LuauOpcode opcode = LOP_SETLIST;
    INT_IMM(StartIndex, 0)
    INT_IMM(Count, 1)
    BC_OP(Target, 2)
    static const uint32_t kParamStartInput = 3;
    std::vector<BcOp> params()
    {
        return this->sliceInputs(kParamStartInput);
    }
};

template<typename VmConst = BcVmConst>
struct BcGetImport : public BcInstHelper<VmConst, BcGetImport<VmConst>>
{
    static const LuauOpcode opcode = LOP_GETIMPORT;
    VM_CONST(Import, 0)
    INT_IMM(PathLength, 1)
    static const uint32_t kPathStartInput = 2;
    std::vector<BcOp> importPath()
    {
        return this->sliceInputs(kPathStartInput);
    }
};

template<typename VmConst = BcVmConst>
struct BcConstruct : public BcInstHelper<VmConst, BcConstruct<VmConst>>
{
    static const LuauOpcode opcode = LOP_CONSTRUCT;
    BC_OP(Class, 0)
    INT_IMM(FbSlot, 1)
};

#undef INT_IMM
#undef BC_OP

} // namespace Bytecode
} // namespace Luau
