// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/BytecodeGraph.h"
#include "Luau/BytecodeOps.h"
#include "Luau/DenseHash.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace Luau
{
namespace Bytecode
{

// Inliner limit is conservatively lower to avoid instructions like namecall that effectively use R(A+1) and CALL/RETURN interpreting 255 as -1
constexpr uint8_t kMaxInlinerCombinedStackSize = 250;

template<typename VmConst>
struct CallInliner
{
    BcFunction<VmConst>& caller;
    BcFunction<VmConst>& target;
    BcCallFB<VmConst> call;
    std::vector<BcOp> callParams;
    Reg targetReg;

    uint32_t callerBlocksSizeBeforeInline = 0;
    uint32_t callerInstSizeBeforeInline = 0;
    uint32_t callerVmConstSizeBeforeInline = 0;
    uint32_t callerProtoSizeBeforeInline = 0;
    uint32_t callerUpValSizeBeforeInline = 0;
    uint32_t callerFeedbackSlotSizeBeforeInline = 0;

    std::vector<BcOp> returnOps;
    std::unordered_set<BcOp, BcOpHash> callProjections;
    std::unordered_map<BcOp, std::vector<BcOp>, BcOpHash> varArgMoves;
    std::vector<std::pair<BcOp, BcOp>> returnSites;
    // memoizes target-phi -> caller-phi so a target phi referenced both in a block's phi list and as
    // another phi's operand maps to a single caller phi. Without this, the operand reference would get
    // its own unanchored duplicate that SCCP never visits (it only visits phis listed in a block)
    DenseHashMap<BcOp, BcOp, BcOpHash> mappedPhis;

    CallInliner(BcFunction<VmConst>& caller, BcFunction<VmConst>& target, BcOp callOp)
        : caller(caller)
        , target(target)
        , call(caller.template as<BcCallFB<VmConst>>(callOp))
        , callParams(call.params())
        , targetReg(call.getOutReg())
    {
    }

    bool hasEdge(const BcEdges& edges, BcBlockEdgeKind kind)
    {
        for (auto& e : edges)
            if (e.kind == kind)
                return true;
        return false;
    }

    void addSuccessor(BcRef<BcBlock> from, BcRef<BcBlock> to, BcBlockEdgeKind kind)
    {
        LUAU_ASSERT(
            kind != BcBlockEdgeKind::Fallthrough ||
            (!hasEdge(from->successors, BcBlockEdgeKind::Fallthrough) && !hasEdge(to->predecessors, BcBlockEdgeKind::Fallthrough))
        );
        from->successors.push_back({kind, to.op});
        to->predecessors.push_back({kind, from.op});
    }

    std::pair<BcRef<BcBlock>, BcRef<BcBlock>> splitBlockOnOp(BcOp splitOp)
    {
        // This function splits a block with an instruction in 3 parts:
        // prevBlock - retains all ops before splitOp. it is original block to keep all existing
        // references to block unaffected. Fallstrhough to insnsBlock.
        // insnBlock - freashly created block containing only splitOp. Fallstrhough to nextBlock.
        // nextBlock - optionally created if there are any instruction after splitOp. If new block is created
        // all successor/predcessor relations are migrated to the new block.
        BcRef<BcInst> insn = caller.inst(splitOp);
        LUAU_ASSERT(insn->block.kind == BcOpKind::Block);
        BcRef<BcBlock> prevBlock = caller.block(insn->block);
        LUAU_ASSERT(std::find(prevBlock->ops.begin(), prevBlock->ops.end(), splitOp) != prevBlock->ops.end());
        BcRef<BcBlock> insnBlock = caller.block(caller.addBlock());
        insnBlock->sortkey = prevBlock->sortkey;
        insnBlock->chainkey = prevBlock->chainkey + 1;

        BcRef<BcBlock> nextBlock = caller.block(caller.addBlock());
        nextBlock->sortkey = insnBlock->sortkey;
        nextBlock->chainkey = insnBlock->chainkey + 1;
        while (prevBlock->ops.back() != splitOp)
        {
            BcRef<BcInst> insn = caller.inst(prevBlock->ops.back());
            prevBlock->ops.pop_back();
            nextBlock->ops.push_front(insn.op);
            insn->block = nextBlock.op;
        }
        // Make all successors to point to next block.
        for (BcBlockEdge& e : prevBlock->successors)
        {
            BcRef<BcBlock> succ = caller.block(e.target);
            for (BcBlockEdge& pred : succ->predecessors)
                if (pred.target == prevBlock.op)
                    pred.target = nextBlock.op;
        }
        nextBlock->successors = prevBlock->successors;
        prevBlock->successors.clear();

        addSuccessor(prevBlock, insnBlock, BcBlockEdgeKind::Fallthrough);
        addSuccessor(insnBlock, nextBlock, BcBlockEdgeKind::Fallthrough);

        LUAU_ASSERT(prevBlock->ops.back() == splitOp);
        prevBlock->ops.pop_back();
        insnBlock->ops.push_back(splitOp);
        caller.instOp(splitOp).block = insnBlock.op;

        return {prevBlock, nextBlock};
    }

    BcOp replaceNamecall(BcNamecall<VmConst> namecall, BcRef<BcBlock>& prevBlock)
    {
        // move LOP_NAMECALL to call block
        namecall.prependTo(call->block);
        prevBlock->ops.pop_back();
        LUAU_ASSERT(targetReg == namecall.getOutReg());
        Reg tableReg = namecall.getOutReg() + 1;

        // and replace it with LOP_MOVE + LOP_GETTABLEKS
        BcMove move = BcMove<VmConst>::create(caller);
        move.setSrc(namecall.Table());
        move.setOutReg(tableReg);
        move.appendTo(prevBlock.op);

        // GETTABLEKS can clobber original source register of NAMECALL and put a function closure there
        // But MOVE target already has a table at this point.
        namecall.setTable(move.op());

        BcGetTableKS getTableKS = BcGetTableKS<VmConst>::create(caller);
        getTableKS.setSource(move.op());
        getTableKS.setHint(namecall.Hint());
        getTableKS.setKey(namecall.Key().op.index);
        getTableKS.setOutReg(targetReg);
        getTableKS.appendTo(prevBlock.op);

        return getTableKS.op();
    }

    void appendCmpProto(BcRef<BcBlock>& prevBlock, BcOp targetOp, uint32_t targetProtoId)
    {
        BcCmpProto cmpProto = BcCmpProto<VmConst>::create(caller);
        cmpProto.setClosure(targetOp);
        cmpProto.setProtoId(targetProtoId);
        cmpProto.setFallback(call->block);
        cmpProto.appendTo(prevBlock.op);
        addSuccessor(prevBlock, caller.block(call->block), BcBlockEdgeKind::Branch);
    }

    void allocateBlocks()
    {
        callerBlocksSizeBeforeInline = uint32_t(caller.blocks.size());
        caller.blocks.resize(callerBlocksSizeBeforeInline + target.blocks.size());
    }

    BcOp mapBlockOp(BcOp targetBlock)
    {
        LUAU_ASSERT(targetBlock.kind == BcOpKind::Block);
        return BcOp{BcOpKind::Block, callerBlocksSizeBeforeInline + targetBlock.index};
    }

    void allocateInstructions()
    {
        callerInstSizeBeforeInline = uint32_t(caller.instructions.size());
        caller.instructions.resize(callerInstSizeBeforeInline + target.instructions.size());
    }

    uint32_t mapInstIndex(uint32_t index)
    {
        if (index == kInvalidInstIdx)
            return kInvalidInstIdx;

        return callerInstSizeBeforeInline + index;
    }

    BcOp mapInstOp(BcOp targetInst)
    {
        LUAU_ASSERT(targetInst.kind == BcOpKind::Inst);
        return BcOp{BcOpKind::Inst, mapInstIndex(targetInst.index)};
    }

    uint32_t mapVmConstIndex(uint32_t index)
    {
        return callerVmConstSizeBeforeInline + index;
    }

    BcOp mapVmConstOp(BcOp targetVmConst)
    {
        LUAU_ASSERT(targetVmConst.kind == BcOpKind::VmConst);
        return BcOp{BcOpKind::VmConst, mapVmConstIndex(targetVmConst.index)};
    }

    void allocateVmConsts()
    {
        callerVmConstSizeBeforeInline = uint32_t(caller.constants.size());
        caller.constants.reserve(callerVmConstSizeBeforeInline + target.constants.size());
        for (auto& c : target.constants)
            caller.constants.push_back(c);
    }

    void allocateFeedbackSlots()
    {
        callerFeedbackSlotSizeBeforeInline = uint32_t(caller.feedbackSlots.size());
        caller.feedbackSlots.reserve(callerFeedbackSlotSizeBeforeInline + target.feedbackSlots.size());

        for (BcFeedbackSlot slot : target.feedbackSlots)
        {
            if (slot.kind == LFT_CALLTARGET)
                slot.callTarget.inst = mapInstIndex(slot.callTarget.inst);
            else if (slot.kind == LFT_CONSTRUCT)
                slot.construct.shape = mapVmConstIndex(slot.construct.shape);
            else
                LUAU_ASSERT(!"unknown feedback slot kind");

            caller.feedbackSlots.push_back(slot);
        }
    }

    void allocateProtos()
    {
        callerProtoSizeBeforeInline = uint32_t(caller.protos.size());
        caller.protos.reserve(callerProtoSizeBeforeInline + target.protos.size());
        for (auto p : target.protos)
            caller.protos.push_back(p);
    }

    BcOp mapProtoOp(BcOp targetProtoOp)
    {
        LUAU_ASSERT(targetProtoOp.kind == BcOpKind::VmProto);
        return BcOp{BcOpKind::VmProto, callerProtoSizeBeforeInline + targetProtoOp.index};
    }

    void allocateUpValues()
    {
        callerUpValSizeBeforeInline = caller.nups;
        caller.nups += target.nups;
    }

    BcOp mapUpValueOp(BcOp targetUpval)
    {
        LUAU_ASSERT(targetUpval.kind == BcOpKind::VmUpvalue);
        return BcOp{BcOpKind::VmUpvalue, callerUpValSizeBeforeInline + targetUpval.index};
    }

    void findTargetCallProjections()
    {
        for (uint32_t i = 0; i < caller.projections.size(); i++)
        {
            BcProj& proj = caller.projections[i];
            if (proj.op == call.op())
            {
                BcOp projOp{BcOpKind::Proj, i};
                if (callProjections.count(projOp) > 0)
                    continue;
                returnOps.resize(proj.index + 1);
                BcOp phiOp = caller.addPhi();
                BcRef<BcPhi> phi = caller.phi(phiOp);
                caller.addUse(phi, projOp);
                callProjections.insert(projOp);
                returnOps[proj.index] = phiOp;
            }
        }
    }

    void setReturnOp(uint32_t idx, BcOp op)
    {
        if (idx >= returnOps.size())
            returnOps.resize(idx + 1);

        if (returnOps[idx].kind == BcOpKind::None)
            returnOps[idx] = op;
        else
        {
            if (returnOps[idx].kind != BcOpKind::Phi)
            {
                BcOp phiOp = caller.addPhi();
                BcRef<BcPhi> phi = caller.phi(phiOp);
                caller.addUse(phi, returnOps[idx]);
                returnOps[idx] = phiOp;
            }
            else
            {
                BcRef<BcPhi> phi = caller.phi(returnOps[idx]);
                bool exists = false;
                for (auto phiOp : phi->ops)
                    if (phiOp == op)
                    {
                        exists = true;
                        break;
                    }

                if (!exists)
                    caller.addUse(phi, op);
            }
        }
    }

    void replaceReturn(BcRef<BcBlock>& nextBlock, BcOp callerBlockOp, BcOp targetReturnOp)
    {
        BcRef<BcBlock> callerBlock = caller.block(callerBlockOp);
        BcReturn ret = target.template as<BcReturn<VmConst>>(targetReturnOp);
        // multi-value returns are rejected by migrateBlocks before collecting the site
        LUAU_ASSERT(ret.ReturnCount() >= 0);
        std::vector<BcOp> values = ret.values();
        uint32_t i = 0;
        for (; i < values.size(); i++)
        {
            BcMove move = BcMove<VmConst>::create(caller);
            move.setSrc(mapToCallerOp(values[i]));
            move.setOutReg(targetReg + i);
            move.appendTo(callerBlockOp);
            setReturnOp(i, move.op());
        }
        int callRes = call.ReturnCount();
        LUAU_ASSERT(callRes >= 0);
        for (; i < static_cast<uint32_t>(callRes); i++)
        {
            BcLoadNil<VmConst> loadNil = BcLoadNil<VmConst>::create(caller);
            loadNil.setOutReg(targetReg + i);
            loadNil.appendTo(callerBlockOp);
            setReturnOp(i, loadNil.op());
        }

        callerBlock->successors.push_back({BcBlockEdgeKind::Fallthrough, nextBlock.op});
        nextBlock->predecessors.push_back({BcBlockEdgeKind::Fallthrough, callerBlockOp});
    }

    void replaceGetVarArg(BcOp callerBlockOp, BcOp targetGetVarArgsOp)
    {
        BcGetVarArgs getVarArgs = target.template as<BcGetVarArgs<VmConst>>(targetGetVarArgsOp);
        int count = getVarArgs.ValuesCount();
        if (count < 0)
            count = std::max(0, int(callParams.size() - target.numparams));
        std::vector<BcOp> moves;
        for (int i = 0; i < count; i++)
        {
            if (static_cast<size_t>(target.numparams + i) < callParams.size())
            {
                BcMove move = BcMove<VmConst>::create(caller);
                move.setSrc(callParams[target.numparams + i]);
                move.setOutReg(mapToCallerReg(getVarArgs.startReg() + i));
                move.appendTo(callerBlockOp);
                moves.push_back(move.op());
            }
            else
            {
                BcLoadNil loadNil = BcLoadNil<VmConst>::create(caller);
                loadNil.setOutReg(mapToCallerReg(getVarArgs.startReg() + i));
                loadNil.appendTo(callerBlockOp);
                moves.push_back(loadNil.op());
            }
        }
        varArgMoves[targetGetVarArgsOp] = moves;
    }

    BcOp getVarArgParam(BcGetVarArgs<VmConst>& getVarArgs, uint32_t idx)
    {
        LUAU_ASSERT(varArgMoves.count(getVarArgs.op()) > 0 && idx < varArgMoves[getVarArgs.op()].size());
        return varArgMoves[getVarArgs.op()][idx];
    }

    bool migrateBlocks(BcRef<BcBlock>& nextBlock)
    {
        BcRef<BcBlock> callBlock = caller.block(call->block);
        uint32_t insnBlockSortKey = callBlock->sortkey;
        uint32_t insnBlockChainKey = callBlock->chainkey;
        uint32_t maxChainKey = 0;
        for (uint32_t i = 0; i < target.blocks.size(); i++)
        {
            BcBlock& targetBlock = target.blocks[i];
            BcBlock& callerBlock = caller.blocks[callerBlocksSizeBeforeInline + i];
            BcOp callerBlockOp = BcOp{BcOpKind::Block, callerBlocksSizeBeforeInline + i};

            if (i == target.exitBlock.index)
            {
                // it is old exit block
                callerBlock.sortkey = kBlockNoStartPc;
                callerBlock.flags |= BcBlockFlag::Dead;
                continue;
            }
            callerBlock.sortkey = insnBlockSortKey;
            callerBlock.chainkey = insnBlockChainKey + targetBlock.sortkey;
            maxChainKey = std::max(callerBlock.chainkey, maxChainKey);
            for (auto& e : targetBlock.successors)
                if (e.target != target.exitBlock)
                    callerBlock.successors.push_back({e.kind, mapBlockOp(e.target)});

            for (auto& e : targetBlock.predecessors)
                callerBlock.predecessors.push_back({e.kind, mapBlockOp(e.target)});

            for (auto op : targetBlock.ops)
            {
                BcInst& inst = target.instOp(op);
                if (inst.op == LOP_GETVARARGS)
                {
                    replaceGetVarArg(callerBlockOp, op);
                }
                else if (inst.op == LOP_RETURN)
                {
                    if (target.template as<BcReturn<VmConst>>(op).ReturnCount() < 0)
                        return false;
                    returnSites.push_back({callerBlockOp, op});
                }
                else if (inst.op != LOP_PREPVARARGS)
                {
                    BcOp callerInstOp = mapInstOp(op);
                    callerBlock.appendInstruction(callerInstOp);
                    caller.instOp(callerInstOp).block = callerBlockOp;
                }
            }
        }
        callBlock->chainkey = maxChainKey + 1;
        nextBlock->chainkey = maxChainKey + 2;
        return true;
    }

    BcOp mapToCallerOp(BcOp targetOp)
    {
        switch (targetOp.kind)
        {
        case BcOpKind::Inst:
            return mapInstOp(targetOp);
        case BcOpKind::Block:
            return mapBlockOp(targetOp);
        case BcOpKind::Imm:
        {
            caller.immediates.push_back(target.immOp(targetOp));
            return BcOp{BcOpKind::Imm, static_cast<uint32_t>(caller.immediates.size() - 1)};
        }
        case BcOpKind::Phi:
        {
            // memoize before recursing so a phi that (transitively) references itself, as loop-carried
            // phis do, resolves to the same caller phi instead of recursing forever
            if (auto it = mappedPhis.find(targetOp); it != nullptr)
                return *it;

            BcOp callerPhiOp = caller.addPhi();
            mappedPhis[targetOp] = callerPhiOp;

            // loop-carried phis have a register we need to preserve as well
            if (auto it = target.regs.find(targetOp); it != target.regs.end())
            {
                caller.regs[callerPhiOp] = mapToCallerReg(it->second);
            }

            BcRef<BcPhi> targetPhi = target.phi(targetOp);
            for (uint32_t i = 0; i < targetPhi->ops.size(); i++)
            {
                BcOp targetPhiOp = targetPhi->ops[i];
                BcOp mapped = mapToCallerOp(targetPhiOp);
                BcRef<BcPhi> callerPhi = caller.phi(callerPhiOp);
                caller.addUse(callerPhi, mapped);
            }

            return callerPhiOp;
        }
        case BcOpKind::Proj:
        {
            BcRef<BcProj> proj = target.proj(targetOp);
            if (target.is_vararg)
            {
                BcRef<BcInst> inst = target.inst(proj->op);
                if (inst->op == LOP_GETVARARGS)
                {
                    BcGetVarArgs<VmConst> getVarArgs = BcGetVarArgs<VmConst>::from(target, inst);
                    LUAU_ASSERT(getVarArgs.ValuesCount() >= 0);
                    return getVarArgParam(getVarArgs, proj->index);
                }
            }
            return caller.addProj(mapToCallerOp(proj->op), proj->index);
        }
        case BcOpKind::VmReg:
            if (targetOp.index < target.numparams)
            {
                // it is an argument for target. we can find it in the call's inputs
                LUAU_ASSERT(targetOp.index < callParams.size());
                return callParams[targetOp.index];
            }
            else
            {
                return BcOp{BcOpKind::VmReg, static_cast<uint32_t>(mapToCallerReg(targetOp.index))};
            }
        case BcOpKind::VmConst:
            return mapVmConstOp(targetOp);
        case BcOpKind::VmProto:
            return mapProtoOp(targetOp);
        case BcOpKind::VmUpvalue:
            return mapUpValueOp(targetOp);
        default:
            return targetOp;
        }
    }

    Reg mapToCallerReg(Reg reg)
    {
        Reg callerReg = targetReg + 1 + (target.is_vararg ? static_cast<uint8_t>(callParams.size()) : 0) + reg;
        LUAU_ASSERT(callerReg < caller.maxstacksize);
        return callerReg;
    }

    bool isMultiConsumer(BcFunction<VmConst>& graph, BcRef<BcInst>& inst)
    {
        switch (inst->op)
        {
        case LOP_SETLIST:
            return BcSetList<VmConst>::from(graph, inst).Count() < 0;
        case LOP_RETURN:
            return BcReturn<VmConst>::from(graph, inst).ReturnCount() < 0;
        case LOP_CALLFB:
            return BcCallFB<VmConst>::from(graph, inst).ParamCount() < 0;
        case LOP_CALL:
            return BcCall<VmConst>::from(graph, inst).ParamCount() < 0;
        default:
            return false;
        }
    }

    void makeFixedConsumer(BcFunction<VmConst>& graph, BcRef<BcInst>& inst)
    {
        switch (inst->op)
        {
        case LOP_SETLIST:
        {
            auto setList = BcSetList<VmConst>::from(graph, inst);
            uint32_t count = static_cast<uint32_t>(setList.params().size());
            if (count == 0)
                setList.detach();
            else
                setList.setCount(count);
            break;
        }
        case LOP_RETURN:
        {
            auto ret = BcReturn<VmConst>::from(graph, inst);
            ret.setReturnCount(static_cast<uint32_t>(ret.values().size()));
            break;
        }
        case LOP_CALLFB:
        {
            auto callFb = BcCallFB<VmConst>::from(graph, inst);
            callFb.setParamCount(static_cast<uint32_t>(callFb.params().size()));
            break;
        }
        case LOP_CALL:
        {
            auto call = BcCall<VmConst>::from(graph, inst);
            call.setParamCount(static_cast<uint32_t>(call.params().size()));
            break;
        }
        default:
            LUAU_UNREACHABLE();
        }
    }

    bool isGetVarArg(BcOp targetOp)
    {
        if (targetOp.kind != BcOpKind::Inst)
            return false;
        BcRef<BcInst> inst = target.inst(targetOp);
        return inst->op == LOP_GETVARARGS;
    }

    void migrateBlockPhis()
    {
        // This pass cannot be joined with migrateBlocks.
        // The reason is phi can refer GETVARARGS instruction's projection.
        // mapToCallerOp tries to map it to corresponding MOVE or LOADNIL,
        // but they are only materialized during migrateBlocks pass.
        for (uint32_t i = 0; i < target.blocks.size(); i++)
        {
            BcBlock& targetBlock = target.blocks[i];
            BcBlock& callerBlock = caller.blocks[callerBlocksSizeBeforeInline + i];

            for (auto phiOp : targetBlock.phis)
            {
                BcOp callerPhiOp = mapToCallerOp(phiOp);
                callerBlock.phis.push_back(callerPhiOp);
            }
        }
    }

    void migrateInstructions()
    {
        for (uint32_t i = 0; i < target.instructions.size(); i++)
        {
            BcOp targetInsnOp{BcOpKind::Inst, i};
            BcOp callerInsnOp{BcOpKind::Inst, callerInstSizeBeforeInline + i};
            BcRef<BcInst> targetInst = target.inst(targetInsnOp);
            BcRef<BcInst> callerInst = caller.inst(callerInsnOp);

            if (targetInst->op == LOP_RETURN || targetInst->op == LOP_GETVARARGS || targetInst->op == LOP_PREPVARARGS)
                continue;

            callerInst->op = targetInst->op;
            callerInst->block = mapBlockOp(targetInst->block);
            callerInst->line = call->line;

            if (target.is_vararg && isMultiConsumer(target, targetInst) && isGetVarArg(targetInst->ops.back()))
            {
                for (BcOp inp : targetInst->ops)
                {
                    if (inp != targetInst->ops.back())
                        caller.addUse(callerInst, mapToCallerOp(inp));
                    else
                    {
                        LUAU_ASSERT(varArgMoves.count(inp) > 0);
                        std::vector<BcOp>& moves = varArgMoves[inp];
                        for (BcOp move : moves)
                            caller.addUse(callerInst, move);
                    }
                }
                makeFixedConsumer(caller, callerInst);
            }
            else
            {
                for (BcOp inp : targetInst->ops)
                    caller.addUse(callerInst, mapToCallerOp(inp));
            }
            if (auto it = target.regs.find(targetInsnOp); it != target.regs.end())
                caller.regs[callerInsnOp] = mapToCallerReg(it->second);
            // Instructions with special migration handling.
            switch (callerInst->op)
            {
            case LOP_CALLFB:
            {
                // Feedback slots are concatenated in optimized version: caller's slots + target's slots.
                // So all target's slot should be increased by caller's slots count.
                BcCallFB<VmConst> fbcall = BcCallFB<VmConst>::from(caller, callerInst);

                // do not migrate sealed fbcalls
                if (fbcall.FbSlot() != -1)
                    fbcall.setFbSlot(fbcall.FbSlot() + callerFeedbackSlotSizeBeforeInline);

                break;
            }
            case LOP_CONSTRUCT:
            {
                BcConstruct<VmConst> construct = BcConstruct<VmConst>::from(caller, callerInst);
                construct.setFbSlot(construct.FbSlot() + callerFeedbackSlotSizeBeforeInline);
                break;
            }
            default:
                break;
            }
        }
    }

    void replaceCallUsagesInOps(BcOp consumer, BcOps& ops)
    {
        // It is safe to assume the call instruction is always referred as a projection,
        // because inlining of only fixed return size calls are supported and parsers
        // always emits projections for fixed return calls.
        // If multireturn calls inlining will be supported in the future, it should account
        // for bare call replacements as well.
        for (BcOp& op : ops)
            if (auto it = callProjections.find(op); it != callProjections.end())
            {
                BcProj& proj = caller.projOp(*it);
                LUAU_ASSERT(proj.index < returnOps.size());
                // the projection operand is rewritten in place, so the `ops` edge already exists;
                // only the return def's reverse `uses` edge needs to be recorded
                op = returnOps[proj.index];
                caller.recordUse(op, consumer);
            }
    }

    void replaceCallUsagesWithReturnPhis()
    {
        for (uint32_t i = 0; i < callerInstSizeBeforeInline; i++)
            replaceCallUsagesInOps(BcOp{BcOpKind::Inst, i}, caller.instructions[i].ops);

        for (uint32_t i = 0; i < caller.phis.size(); i++)
            if (std::find(returnOps.begin(), returnOps.end(), BcOp{BcOpKind::Phi, i}) == returnOps.end())
                replaceCallUsagesInOps(BcOp{BcOpKind::Phi, i}, caller.phis[i].ops);
    }

    void allocateGraphEntitiesForTarget()
    {
        allocateBlocks();
        allocateInstructions();
        allocateVmConsts();
        allocateFeedbackSlots();
        allocateProtos();
        allocateUpValues();
    }

    void setFallthrough(BcEdges& edges, BcOp entry)
    {
        for (auto& e : edges)
            if (e.kind == BcBlockEdgeKind::Fallthrough)
            {
                e.target = entry;
                return;
            }
        edges.push_back({BcBlockEdgeKind::Fallthrough, entry});
    }

    void fillUnderCallArguments()
    {
        if (callParams.size() >= target.numparams)
            return;

        BcOp inlineEntryBlock = mapBlockOp(target.entryBlock);
        size_t callParamSize = callParams.size();
        callParams.resize(target.numparams);
        for (Reg param = target.numparams; param > callParamSize; param--)
        {
            BcLoadNil<VmConst> loadNil = BcLoadNil<VmConst>::create(caller);
            loadNil.setOutReg(targetReg + param);
            loadNil.prependTo(inlineEntryBlock);
            callParams[param - 1] = loadNil.op();
        }
    }

    bool inlineTarget(uint32_t targetProtoId)
    {
        LUAU_ASSERT(validate());
        uint32_t newMaxStackSize = static_cast<uint32_t>(caller.maxstacksize) + static_cast<uint32_t>(target.maxstacksize);

        if (target.is_vararg)
            // If there are less call arguments than target.numparams it will be filled with nils.
            newMaxStackSize += std::max(uint8_t(callParams.size()), target.numparams);

        if (newMaxStackSize >= kMaxInlinerCombinedStackSize)
            return false;

        if (call.ParamCount() < 0 || call.ReturnCount() < 0)
            return false;

        // inlining of upvalues is not supported yet
        if (target.nups > 0)
            return false;

        caller.maxstacksize = newMaxStackSize;

        auto [prevBlock, nextBlock] = splitBlockOnOp(call.op());

        BcOp targetOp = call.Target();
        if (prevBlock->ops.size() > 0)
        {
            auto lastInst = caller.inst(prevBlock->ops.back());
            if (lastInst->op == LOP_NAMECALL)
                targetOp = replaceNamecall(caller.template as<BcNamecall<VmConst>>(lastInst.op), prevBlock);
        }

        appendCmpProto(prevBlock, targetOp, targetProtoId);

        // Seal FB slot of inlined call.
        call.setFbSlot(-1);

        allocateGraphEntitiesForTarget();

        fillUnderCallArguments();

        findTargetCallProjections();

        if (!migrateBlocks(nextBlock))
            return false;

        BcRef<BcBlock> callerInlinedEntry = caller.block(mapBlockOp(target.entryBlock));

        // Remove prevBlock fallthrough to call block from its predecessors
        BcEdges& insnPreds = caller.block(call->block)->predecessors;
        insnPreds.resize(
            unsigned(
                std::remove_if(
                    insnPreds.begin(),
                    insnPreds.end(),
                    [prevBlock = prevBlock](BcBlockEdge& p)
                    {
                        return p.kind == BcBlockEdgeKind::Fallthrough && p.target == prevBlock.op;
                    }
                ) -
                insnPreds.begin()
            )
        );

        setFallthrough(prevBlock->successors, callerInlinedEntry.op);
        setFallthrough(callerInlinedEntry->predecessors, prevBlock.op);

        migrateBlockPhis();

        for (auto& [callerBlockOp, targetReturnOp] : returnSites)
            replaceReturn(nextBlock, callerBlockOp, targetReturnOp);

        migrateInstructions();

        replaceCallUsagesWithReturnPhis();

        for (BcOp retOp : returnOps)
            if (retOp.kind == BcOpKind::Phi)
                nextBlock->phis.push_back(retOp);

        LUAU_ASSERT(validate());

        return true;
    }

    bool validate() const
    {
        if (!validateCfg())
            return false;
        if (!validatePhis())
            return false;
        return true;
    }

    bool validatePhis() const
    {
        for (BcPhi& phi : caller.phis)
            for (BcOp op : phi.ops)
                LUAU_ASSERT(op.kind == BcOpKind::Inst || op.kind == BcOpKind::VmReg || op.kind == BcOpKind::Proj || op.kind == BcOpKind::Phi);
        return true;
    }

    bool validateCfg() const
    {
        auto validateEdges = [&](uint32_t from, const BcEdges& edges, const BcEdges BcBlock::* mirrorDir) -> bool
        {
            for (const BcBlockEdge& edge : edges)
            {
                // In-range
                if (edge.target.kind != BcOpKind::Block || edge.target.index >= caller.blocks.size())
                    return false;

                const BcBlock& other = caller.blocks[edge.target.index];

                // Alive
                if ((other.flags & BcBlockFlag::Dead) != 0)
                    return false;

                // Outgoing edge is mirrored in the target
                const BcEdges& mirror = other.*mirrorDir;
                if (std::find_if(
                        mirror.begin(),
                        mirror.end(),
                        [&](const BcBlockEdge& e)
                        {
                            return e.kind == edge.kind && e.target.kind == BcOpKind::Block && e.target.index == from;
                        }
                    ) == mirror.end())
                    return false;
            }

            return true;
        };

        for (uint32_t i = 0; i < caller.blocks.size(); i++)
        {
            const BcBlock& block = caller.blocks[i];

            // Skip dead blocks as they might be in inconsistent state
            if ((block.flags & BcBlockFlag::Dead) != 0)
                continue;

            if (!validateEdges(i, block.successors, &BcBlock::predecessors))
                return false;

            if (!validateEdges(i, block.predecessors, &BcBlock::successors))
                return false;
        }

        return true;
    }
};

template<typename VmConst>
bool inlineCall(BcFunction<VmConst>& caller, BcFunction<VmConst>& target, BcOp callOp, uint32_t targetProtoId)
{
    CallInliner<VmConst> inliner(caller, target, callOp);
    return inliner.inlineTarget(targetProtoId);
}

} // namespace Bytecode
} // namespace Luau
