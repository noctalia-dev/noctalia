// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/BytecodeGraph.h"
#include "Luau/BytecodeUtils.h"
#include "Luau/Common.h"
#include "Luau/InsertionOrderedMap.h"

#include <algorithm>
#include <optional>
#include <utility>

LUAU_FASTFLAG(DebugLuauUserDefinedClasses)

namespace Luau
{
namespace Bytecode
{

template<typename VmConst>
struct BytecodeGraphParser
{
    struct BlockProducers
    {
        std::unordered_map<Reg, BcOp> own;
        std::unordered_map<Reg, BcOp> cached;
        BcOp multiReturn;
        Reg multiReturnStart;
        int invalidAfter = 255;
        // incomplete phi construction state:
        // - a block is sealed once all its predecessors have been emitted
        // - reads that cross a not-yet-emitted predecessor (a back-edge)
        //   create an operand-less phi in incompletePhis until the block is sealed and the operands can be filled
        // - incomplete phis are filled in with operands that all occupy the same register, after all preds have been emitted
        //
        // this enables loop phis to be created without a separate pass (Braun "Simple and Efficient Construction of Static Single Assignment Form")
        bool sealed = false;
        uint32_t unsealedPreds = 0;
        InsertionOrderedMap<Reg, BcOp> incompletePhis;
    };

    using Producers = std::vector<BlockProducers>;

    BcFunction<VmConst>& func;
    std::unordered_map<uint32_t, BcOp> blockByPC;
    Producers producers;
    BcOp currentBlock;
    std::unordered_map<BcOp, BcOp, BcOpHash> phiBlock;

    BytecodeGraphParser(BcFunction<VmConst>& func)
        : func(func)
    {
    }

    void addSuccessor(BcOp fromOp, BcOp toOp, BcBlockEdgeKind kind)
    {
        BcBlock& from = func.blockOp(fromOp);
        BcBlock& to = func.blockOp(toOp);
        from.successors.push_back({kind, toOp});
        to.predecessors.push_back({kind, fromOp});
    }

    BcOp makeBlock(uint32_t pc)
    {
        BcOp newBlockOp = func.addBlock();
        blockByPC[pc] = newBlockOp;
        BcBlock& newBlock = func.blockOp(newBlockOp);
        newBlock.sortkey = pc;
        return newBlockOp;
    }

    bool isJumpTrampoline(uint32_t pc, const Instruction* code, uint32_t codesize)
    {
        return LuauOpcode(LUAU_INSN_OP(code[pc])) == LOP_JUMP && pc + 1 < codesize && LuauOpcode(LUAU_INSN_OP(code[pc + 1])) == LOP_JUMPX &&
               static_cast<uint32_t>(getJumpTarget(code[pc + 2], pc + 2)) == pc + 1;
    }

    size_t rebuildBlocks(const Instruction code[], uint32_t codesize)
    {
        BcOp entryBlock = func.entryBlock = makeBlock(0);
        BcOp exitBlock = func.exitBlock = makeBlock(kBlockNoStartPc);
        uint32_t i = 0;
        BcOp currentBlock = entryBlock;
        size_t instructionCount = 0;
        while (i < codesize)
        {
            Instruction insn = code[i];
            LuauOpcode op = LuauOpcode(LUAU_INSN_OP(insn));
            int target = getJumpTarget(insn, i);
            if (target >= 0 && LuauOpcode(LUAU_INSN_OP(code[target])) == LOP_JUMPX)
                target = getJumpTarget(code[target], target);

            bool needsBlock = target >= 0 && !isFastCall(op) && op != LOP_JUMPX && !isJumpTrampoline(i, code, codesize);
            if (needsBlock)
            {
                if (blockByPC.count(target) == 0)
                {
                    BcOp newBlockOp = makeBlock(target);
                    if (target < static_cast<int>(i)) // We are jumping back.
                    {
                        // The new block was created in the middle of the existing one.
                        // We need to maintain predecessor/successor relations.
                        uint32_t blockStartPc = target - 1;
                        while (blockByPC.count(blockStartPc) == 0 && blockStartPc-- != 0)
                            ;
                        LUAU_ASSERT(blockByPC.count(blockStartPc) > 0);
                        BcOp prevBlockOp = blockByPC[blockStartPc];
                        BcBlock& prevBlock = func.blockOp(prevBlockOp);
                        BcBlock& newBlock = func.blockOp(newBlockOp);
                        // Steal successors of the previous block.
                        newBlock.successors = prevBlock.successors;
                        // Now it should only fallsthrough to the new block.
                        prevBlock.successors.clear();
                        addSuccessor(prevBlockOp, newBlockOp, BcBlockEdgeKind::Fallthrough);
                        // Update all successors to have the new block as a predecessor instead of the old one.
                        for (auto& edge : newBlock.successors)
                            for (auto& backEdge : func.blockOp(edge.target).predecessors)
                                if (backEdge.target == prevBlockOp)
                                    backEdge.target = newBlockOp;
                    }
                }
                addSuccessor(currentBlock, blockByPC[target], isLoopJump(op) ? BcBlockEdgeKind::Loop : BcBlockEdgeKind::Branch);
            }
            if (op == LOP_RETURN)
                addSuccessor(currentBlock, exitBlock, BcBlockEdgeKind::Fallthrough);
            i += getOpLength(op);
            if ((needsBlock || (op == LOP_RETURN && i < codesize)) && blockByPC.count(i) == 0)
                makeBlock(i);

            if (blockByPC.count(i) != 0)
            {
                if (isFallthrough(op))
                    addSuccessor(currentBlock, blockByPC[i], BcBlockEdgeKind::Fallthrough);
                currentBlock = blockByPC[i];
            }
            instructionCount++;
        }
        return instructionCount;
    }

    BcOp makePhi(BcOp block, Reg reg)
    {
        BcOp phiOp = func.addPhi();
        func.regs[phiOp] = reg;
        func.blockOp(block).phis.push_back(phiOp);
        phiBlock[phiOp] = block;
        return phiOp;
    }

    std::optional<BcOp> readVariable(BcOp block, Reg reg)
    {
        BlockProducers& bp = producers.at(block.index);
        if (static_cast<int>(reg) > bp.invalidAfter)
            return {};
        if (auto it = bp.own.find(reg); it != bp.own.end())
            return it->second;
        if (auto it = bp.cached.find(reg); it != bp.cached.end())
            return it->second;
        if (bp.multiReturn.kind != BcOpKind::None && reg >= bp.multiReturnStart)
        {
            // cache the projection so repeated reads (and phi operands across edges) share identity,
            // which keeps tryRemoveTrivialPhi able to recognize equal operands
            BcOp proj = func.addProj(bp.multiReturn, reg - bp.multiReturnStart);
            bp.cached[reg] = proj;
            return proj;
        }
        return readVariableRecursive(block, reg);
    }

    BcOp readVariableRecursive(BcOp block, Reg reg)
    {
        BlockProducers& bp = producers.at(block.index);
        BcEdges& preds = func.blockOp(block).predecessors;

        if (!bp.sealed)
        {
            // predecessors not all emitted yet, so we create an incomplete phi to fill on seal
            BcOp phiOp = makePhi(block, reg);
            bp.incompletePhis[reg] = phiOp;
            bp.cached[reg] = phiOp;
            return phiOp;
        }

        if (preds.empty())
            return BcOp{BcOpKind::VmReg, reg}; // undefined (entry/unreachable)

        if (preds.size() == 1)
        {
            // No phi needed, as a single-predecessor block can never a loop header so this can only cycle inside a fully-unreachable single-pred loop
            // we cache anyway so if we have a re-entry, we just resolve to undef, rather than recursing forever
            bp.cached[reg] = BcOp{BcOpKind::VmReg, reg};
            BcOp val = readVariable(preds[0].target, reg).value_or(BcOp{BcOpKind::VmReg, reg});
            producers.at(block.index).cached[reg] = val;
            return val;
        }

        // multiple predecessors: create the phi and cache it *before* filling so a back-edge read
        // that recurses back into this block resolves to the phi instead of looping forever
        BcOp phiOp = makePhi(block, reg);
        bp.cached[reg] = phiOp;
        BcOp val = addPhiOperands(reg, phiOp, block);
        producers.at(block.index).cached[reg] = val;
        return val;
    }

    BcOp addPhiOperands(Reg reg, BcOp phiOp, BcOp block)
    {
        LUAU_ASSERT(phiOp.kind == BcOpKind::Phi);

        BcRef<BcPhi> phi = func.phi(phiOp);

        // include every predecessor edge, back-edges included
        for (auto& [_, pred] : func.blockOp(block).predecessors)
        {
            if (std::optional<BcOp> v = readVariable(pred, reg))
            {
                func.addUse(phi, *v);
            }
        }
        return tryRemoveTrivialPhi(phiOp);
    }

    // collapse a phi whose operands (ignoring self-references) are a single distinct value
    // NOTE: a self-referential loop phi left in place would make the serializer's getRegister recurse forever
    BcOp tryRemoveTrivialPhi(BcOp phiOp)
    {
        std::optional<BcOp> trivialValue = std::nullopt;
        for (BcOp op : func.phiOp(phiOp).ops)
        {
            if (op == phiOp || (trivialValue.has_value() && op == *trivialValue))
                continue;

            if (trivialValue.has_value())
                return phiOp; // two distinct values, we cannot eliminate this

            trivialValue = op;
        }

        Reg reg = static_cast<Reg>(func.regs.at(phiOp));
        if (!trivialValue.has_value())
            trivialValue = BcOp{BcOpKind::VmReg, reg}; // unreachable or undefined, so we collapse to an VmReg read

        BcRef<BcPhi> phiRef = func.phi(phiOp);
        std::vector<BcOp> users = std::move(phiRef->uses);

        // we need to now update users to point to the new trivial value
        for (BcOp user : users)
        {
            if (user == phiOp)
                continue;

            BcOps& userOps = (user.kind == BcOpKind::Phi) ? func.phiOp(user).ops : func.instOp(user).ops;
            for (BcOp& op : userOps)
                if (op == phiOp)
                {
                    op = *trivialValue;

                    // re-record the reverse edge on whichever def now owns this operand
                    func.recordUse(*trivialValue, user);
                }
        }

        // remove the collapsed phi from its block
        if (auto bit = phiBlock.find(phiOp); bit != phiBlock.end())
        {
            func.blockOp(bit->second).phis.remove(phiOp);
            BlockProducers& bp = producers.at(bit->second.index);
            if (auto cit = bp.cached.find(reg); cit != bp.cached.end() && cit->second == phiOp)
                cit->second = *trivialValue;
            phiBlock.erase(bit);
        }

        for (BcOp user : users)
            if (user.kind == BcOpKind::Phi && user != phiOp)
                tryRemoveTrivialPhi(user);

        return *trivialValue;
    }

    void sealBlock(BcOp block)
    {
        BlockProducers& bp = producers.at(block.index);
        if (bp.sealed)
            return;
        // mark sealed first so reads triggered while filling use the normal (non-incomplete) path
        bp.sealed = true;

        InsertionOrderedMap<Reg, BcOp> pending = std::move(bp.incompletePhis);
        bp.incompletePhis.clear();

        for (const auto& [reg, phiOp] : pending)
        {
            BcOp val = addPhiOperands(reg, phiOp, block);
            BlockProducers& cur = producers.at(block.index);
            if (auto cit = cur.cached.find(reg); cit != cur.cached.end() && cit->second == phiOp)
                cit->second = val;
        }
    }

    void finalizeBlock(BcOp block)
    {
        for (auto& [_, succ] : func.blockOp(block).successors)
        {
            BlockProducers& sp = producers.at(succ.index);
            if (sp.unsealedPreds > 0)
            {
                --sp.unsealedPreds;
                if (sp.unsealedPreds == 0)
                    sealBlock(succ);
            }
        }
    }

    void sealAllRemaining()
    {
        for (uint32_t b = 0; b < func.blocks.size(); b++)
            if (!producers.at(b).sealed)
                sealBlock(BcOp{BcOpKind::Block, b});
    }

    std::vector<BcOp> findProducersUpToTop(BcOp block, Reg reg)
    {
        // We assume it called only for search of var return calls.
        LUAU_ASSERT(block.index < producers.size());
        BlockProducers& blockProducers = producers.at(block.index);
        // So we need to find all producers from reg to blockProducers.multiReturnStart.
        LUAU_ASSERT(blockProducers.multiReturn.kind == BcOpKind::Inst);
        std::vector<BcOp> res;
        res.reserve(blockProducers.multiReturnStart - reg + 1);
        for (; reg < blockProducers.multiReturnStart; reg++)
        {
            auto staticRegOp = readVariable(block, reg);
            LUAU_ASSERT(staticRegOp);
            res.push_back(*staticRegOp);
        }
        res.push_back(blockProducers.multiReturn);
        // multireturn is consumed, clean it up
        blockProducers.multiReturn = BcOp{};
        blockProducers.multiReturnStart = 0xFF;
        return res;
    }

    bool isUnreachable(BcOp blockOp)
    {
        if (blockOp == func.entryBlock)
            return false;
        BcBlock& block = func.blockOp(blockOp);
        for (auto [ctrl, pred] : block.predecessors)
        {
            if (ctrl == BcBlockEdgeKind::Loop)
                continue;
            if (!isUnreachable(pred))
                return false;
        }
        return true;
    }

    void addProducer(Reg reg, BcOp op)
    {
        BlockProducers& blockProducers = producers[currentBlock.index];
        blockProducers.own[reg] = op;
        func.regs[op] = reg;
        blockProducers.invalidAfter = std::max(static_cast<int>(reg), blockProducers.invalidAfter);
    }

    void applyCall(BlockProducers& producers, BcOp callOp, Reg targetReg, int nresults)
    {
        for (auto it = producers.own.begin(); it != producers.own.end();)
        {
            if (it->first >= targetReg)
            {
                it = producers.own.erase(it);
            }
            else
            {
                ++it;
            }
        }
        for (auto it = producers.cached.begin(); it != producers.cached.end();)
        {
            if (it->first >= targetReg)
            {
                it = producers.cached.erase(it);
            }
            else
            {
                ++it;
            }
        }
        if (nresults < 0)
        {
            producers.multiReturn = callOp;
            producers.multiReturnStart = targetReg;
            producers.invalidAfter = 255;
        }
        else
        {
            producers.invalidAfter = static_cast<int>(targetReg) - 1 + nresults;
        }
    }

    void addImmInput(BcRef<BcInst> inst, bool value)
    {
        BcOp op{BcOpKind::Imm, 0};
        func.immediates.push_back({BcImmKind::Boolean});
        func.immediates.back().valueBoolean = value;
        op.index = func.immediates.size() - 1;
        func.addUse(inst, op);
    }

    void addImmInput(BcRef<BcInst> inst, int32_t value)
    {
        BcOp op{BcOpKind::Imm, 0};
        func.immediates.push_back({BcImmKind::Int});
        func.immediates.back().valueInt = value;
        op.index = func.immediates.size() - 1;
        func.addUse(inst, op);
    }

    void addImmInput(BcRef<BcInst> inst, uint32_t value)
    {
        BcOp op{BcOpKind::Imm, 0};
        func.immediates.push_back({BcImmKind::Import});
        func.immediates.back().valueImport = value;
        op.index = func.immediates.size() - 1;
        func.addUse(inst, op);
    }

    void addVmConstInput(BcRef<BcInst> inst, uint32_t idx)
    {
        LUAU_ASSERT(idx < func.constants.size());
        func.addUse(inst, BcOp{BcOpKind::VmConst, idx});
    }

    void addUpvalInput(BcRef<BcInst> inst, uint32_t idx)
    {
        LUAU_ASSERT(idx < func.nups);
        func.addUse(inst, BcOp{BcOpKind::VmUpvalue, idx});
    }

    void addProtoInput(BcRef<BcInst> inst, uint32_t idx)
    {
        func.addUse(inst, BcOp{BcOpKind::VmProto, idx});
    }

    void addVmRegInput(BcRef<BcInst> inst, Reg reg)
    {
        std::optional<BcOp> source = readVariable(currentBlock, reg);
        if (!source && isUnreachable(currentBlock))
        {
            func.addUse(inst, BcOp{BcOpKind::VmReg, reg});
            return;
        }
        LUAU_ASSERT(source);
        func.addUse(inst, *source);
    }

    void addJumpInput(BcRef<BcInst> inst, int target)
    {
        LUAU_ASSERT(!isFastCall(inst->op));
        if (target < 0)
        {
            LUAU_ASSERT(inst->op == LOP_LOADB);
            return;
        }
        auto it = blockByPC.find(target);
        LUAU_ASSERT(it != blockByPC.end());
        func.addUse(inst, it->second);
    }

    void addEmptyInput(BcRef<BcInst> inst)
    {
        func.addUse(inst, BcOp{BcOpKind::None, 0});
    }

    static const uint32_t kMaxCFGBlocks = 1000;

    bool rebuildGraph(const Instruction code[], uint32_t codesize, std::vector<uint32_t>& lines, std::vector<uint32_t>& pcs)
    {
        size_t instructionsCount = rebuildBlocks(code, codesize);
        if (blockByPC.size() > kMaxCFGBlocks)
            return false;

        producers.resize(func.blocks.size());
        pcs.resize(codesize);

        currentBlock = func.entryBlock;

        for (Reg i = 0; i < func.numparams; i++)
            addProducer(i, {BcOpKind::VmReg, i});

        // a block is sealable once all its predecessors are emitted; seed the counts from the CFG
        // (already fully built by rebuildBlocks) and seal blocks with no predecessors immediately
        for (BcBlock& block : func.blocks)
            producers.at(func.getBlockIndex(block)).unsealedPreds = uint32_t(block.predecessors.size());
        for (uint32_t blockIdx = 0; blockIdx < func.blocks.size(); blockIdx++)
            if (producers.at(blockIdx).unsealedPreds == 0)
                sealBlock(BcOp{BcOpKind::Block, blockIdx});

        // Create instructions.
        currentBlock = func.entryBlock;
        func.instructions.reserve(instructionsCount);

        for (uint32_t i = 0; i < codesize;)
        {
            Instruction insn = code[i];
            LuauOpcode op = LuauOpcode(LUAU_INSN_OP(insn));
            int opLength = getOpLength(op);
            uint32_t aux = (opLength > 1 && i + 1 < codesize) ? code[i + 1] : 0;
            BcOp nodeOp = func.addInst();
            func.blockOp(currentBlock).appendInstruction(nodeOp);
            BcRef<BcInst> node = func.inst(nodeOp);
            node->block = currentBlock;
            if (i < lines.size())
                node->line = lines[i];
            node->op = op;

            pcs[i] = nodeOp.index;

            auto parseJump = [&](LuauOpcode op, int jumpTarget) -> void
            {
                node->op = op;
                switch (op)
                {
                case LOP_JUMPXEQKNIL:
                    addVmRegInput(node, LUAU_INSN_A(insn));
                    addImmInput(node, static_cast<bool>(aux >> 31));
                    addJumpInput(node, jumpTarget);
                    break;

                case LOP_JUMPXEQKB:
                    addVmRegInput(node, LUAU_INSN_A(insn));
                    addImmInput(node, static_cast<bool>(aux >> 31));
                    addJumpInput(node, jumpTarget);
                    addImmInput(node, static_cast<bool>(aux & 0x1));
                    break;

                case LOP_JUMPXEQKN:
                case LOP_JUMPXEQKS:
                    addVmRegInput(node, LUAU_INSN_A(insn));
                    addImmInput(node, static_cast<bool>(aux >> 31));
                    addJumpInput(node, jumpTarget);
                    addVmConstInput(node, aux & 0xFFFFFF);
                    break;

                case LOP_JUMPIF:
                case LOP_JUMPIFNOT:
                    addVmRegInput(node, LUAU_INSN_A(insn));
                    addJumpInput(node, jumpTarget);
                    break;

                case LOP_JUMPIFEQ:
                case LOP_JUMPIFLE:
                case LOP_JUMPIFLT:
                case LOP_JUMPIFNOTEQ:
                case LOP_JUMPIFNOTLE:
                case LOP_JUMPIFNOTLT:
                    addVmRegInput(node, LUAU_INSN_A(insn));
                    addVmRegInput(node, aux);
                    addJumpInput(node, jumpTarget);
                    break;

                case LOP_FORNPREP:
                case LOP_FORGPREP:
                case LOP_FORGPREP_NEXT:
                case LOP_FORGPREP_INEXT:
                    // for loop prep: A..A+2 carry iterator state for both numeric loops and generalized
                    addVmRegInput(node, LUAU_INSN_A(insn));
                    addVmRegInput(node, LUAU_INSN_A(insn) + 1);
                    addVmRegInput(node, LUAU_INSN_A(insn) + 2);
                    addJumpInput(node, jumpTarget);
                    func.regs[nodeOp] = LUAU_INSN_A(insn);
                    for (int offset = 0; offset < 3; ++offset)
                        addProducer(LUAU_INSN_A(insn) + offset, func.addProj(nodeOp, offset));
                    break;

                case LOP_FORNLOOP:
                    addVmRegInput(node, LUAU_INSN_A(insn));
                    addVmRegInput(node, LUAU_INSN_A(insn) + 1);
                    addVmRegInput(node, LUAU_INSN_A(insn) + 2);
                    addJumpInput(node, jumpTarget);
                    func.regs[nodeOp] = LUAU_INSN_A(insn);
                    addProducer(LUAU_INSN_A(insn) + 2, func.addProj(nodeOp, 2));
                    break;

                // FORGLOOP updates A+2 for the iterator register and A+3..A+vars for loop variables
                case LOP_FORGLOOP:
                {
                    addVmRegInput(node, LUAU_INSN_A(insn));
                    addVmRegInput(node, LUAU_INSN_A(insn) + 1);
                    addVmRegInput(node, LUAU_INSN_A(insn) + 2);
                    addImmInput(node, static_cast<bool>(aux >> 31));
                    int32_t vars = aux & 0xFF;
                    addImmInput(node, vars);
                    addJumpInput(node, jumpTarget);
                    func.regs[nodeOp] = LUAU_INSN_A(insn);
                    for (int offset = 2; offset <= 2 + std::max(vars, 2); ++offset)
                        addProducer(LUAU_INSN_A(insn) + offset, func.addProj(nodeOp, offset));
                    break;
                }

                default:
                    LUAU_UNREACHABLE();
                }
            };
            switch (op)
            {
            case LOP_NOP:
            case LOP_BREAK:
            case LOP_NATIVECALL:
                break;

            case LOP_LOADNIL:
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_LOADB:
                addImmInput(node, static_cast<bool>(LUAU_INSN_B(insn)));
                addJumpInput(node, getJumpTarget(insn, i));
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_LOADN:
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_D(insn)));
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_LOADK:
                addVmConstInput(node, LUAU_INSN_D(insn));
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_MOVE:
                addVmRegInput(node, LUAU_INSN_B(insn));
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_GETGLOBAL:
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_C(insn)));
                addVmConstInput(node, aux);
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_SETGLOBAL:
                addVmRegInput(node, LUAU_INSN_A(insn));
                addImmInput(node, static_cast<uint8_t>(LUAU_INSN_C(insn)));
                addVmConstInput(node, aux);
                break;

            case LOP_GETUPVAL:
                addUpvalInput(node, LUAU_INSN_B(insn));
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_SETUPVAL:
                addVmRegInput(node, LUAU_INSN_A(insn));
                addUpvalInput(node, LUAU_INSN_B(insn));
                break;

            case LOP_CLOSEUPVALS:
                func.addUse(node, BcOp{BcOpKind::VmReg, LUAU_INSN_A(insn)});
                break;

            case LOP_GETIMPORT:
            {
                addVmConstInput(node, LUAU_INSN_D(insn));
                int32_t componentsCount = aux >> 30;
                addImmInput(node, componentsCount);
                for (int32_t i = 0; i < componentsCount; i++)
                    addVmConstInput(node, (aux >> (20 - 10 * i)) & 0x3FF);
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;
            }

            case LOP_GETTABLE:
                addVmRegInput(node, LUAU_INSN_B(insn));
                addVmRegInput(node, LUAU_INSN_C(insn));
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_SETTABLE:
                addVmRegInput(node, LUAU_INSN_A(insn));
                addVmRegInput(node, LUAU_INSN_B(insn));
                addVmRegInput(node, LUAU_INSN_C(insn));
                break;

            case LOP_GETUDATAKS:
            case LOP_GETTABLEKS:
                addVmRegInput(node, LUAU_INSN_B(insn));
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_C(insn)));
                if (op == LOP_GETUDATAKS)
                {
                    addVmConstInput(node, LUAU_INSN_AUX_KV16(aux));
                    addImmInput(node, static_cast<int32_t>(LUAU_INSN_AUX_SLOT(aux)));
                }
                else
                    addVmConstInput(node, aux);
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_SETUDATAKS:
            case LOP_SETTABLEKS:
                addVmRegInput(node, LUAU_INSN_A(insn));
                addVmRegInput(node, LUAU_INSN_B(insn));
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_C(insn)));
                if (op == LOP_SETUDATAKS)
                {
                    addVmConstInput(node, LUAU_INSN_AUX_KV16(aux));
                    addImmInput(node, static_cast<int32_t>(LUAU_INSN_AUX_SLOT(aux)));
                }
                else
                    addVmConstInput(node, aux);
                break;

            case LOP_GETTABLEN:
                addVmRegInput(node, LUAU_INSN_B(insn));
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_C(insn) + 1));
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_SETTABLEN:
                addVmRegInput(node, LUAU_INSN_A(insn));
                addVmRegInput(node, LUAU_INSN_B(insn));
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_C(insn) + 1));
                break;

            case LOP_NEWCLOSURE:
                addProtoInput(node, LUAU_INSN_D(insn));
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_NAMECALLUDATA:
            case LOP_NAMECALL:
                addVmRegInput(node, LUAU_INSN_B(insn));
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_C(insn)));
                if (op == LOP_NAMECALLUDATA)
                {
                    addVmConstInput(node, LUAU_INSN_AUX_KV16(aux));
                    addImmInput(node, static_cast<int32_t>(LUAU_INSN_AUX_SLOT(aux)));
                }
                else
                    addVmConstInput(node, aux);
                func.regs[nodeOp] = LUAU_INSN_A(insn);
                addProducer(LUAU_INSN_A(insn), func.addProj(nodeOp, 0));
                addProducer(LUAU_INSN_A(insn) + 1, func.addProj(nodeOp, 1));
                break;

            case LOP_CALL:
            case LOP_CALLFB:
            {
                int nparams = LUAU_INSN_B(insn) - 1;
                int nresults = LUAU_INSN_C(insn) - 1;
                addImmInput(node, static_cast<int32_t>(nparams));
                addImmInput(node, static_cast<int32_t>(nresults));
                if (op == LOP_CALLFB)
                    addImmInput(node, static_cast<int32_t>(aux));

                // Call target.
                addVmRegInput(node, LUAU_INSN_A(insn));
                // Fixed arguments.
                for (int i = 1; i <= nparams; i++)
                    addVmRegInput(node, LUAU_INSN_A(insn) + i);

                if (nparams < 0)
                {
                    // all arguments prepared before call in the same block
                    for (auto& inp : findProducersUpToTop(currentBlock, LUAU_INSN_A(insn) + 1))
                    {
                        func.addUse(node, inp);
                    }
                }

                BlockProducers& blockProducers = producers[currentBlock.index];
                applyCall(blockProducers, nodeOp, LUAU_INSN_A(insn), nresults);

                func.regs[nodeOp] = LUAU_INSN_A(insn);
                for (int i = 0; i < nresults; i++)
                    addProducer(LUAU_INSN_A(insn) + i, func.addProj(nodeOp, i));
                break;
            }

            case LOP_RETURN:
            {
                int nresults = LUAU_INSN_B(insn) - 1;
                addImmInput(node, static_cast<int32_t>(nresults));
                for (int i = 0; i < nresults; i++)
                    addVmRegInput(node, LUAU_INSN_A(insn) + i);
                if (nresults < 0)
                    for (auto& inp : findProducersUpToTop(currentBlock, LUAU_INSN_A(insn)))
                        func.addUse(node, inp);
                if (nresults == 0)
                    func.addUse(node, BcOp{BcOpKind::VmReg, LUAU_INSN_A(insn)});
                break;
            }

            case LOP_JUMP:
            {
                if (isJumpTrampoline(i, code, codesize))
                {
                    // it is long jump trampoline
                    int longOffset = LUAU_INSN_E(code[i + 1]);
                    i += getOpLength(LOP_JUMP) + getOpLength(LOP_JUMPX);
                    op = LuauOpcode(LUAU_INSN_OP(code[i]));
                    opLength = getOpLength(op);
                    aux = (opLength > 1 && i + 1 < codesize) ? code[i + 1] : 0;
                    parseJump(op, i + longOffset);
                }
                else
                    addJumpInput(node, getJumpTarget(insn, i));
                break;
            }

            case LOP_JUMPBACK:
                // repeat .. until loops use it for back edge.
                addJumpInput(node, getJumpTarget(insn, i));
                break;

            case LOP_JUMPXEQKNIL:
            case LOP_JUMPXEQKB:
            case LOP_JUMPXEQKN:
            case LOP_JUMPXEQKS:
            case LOP_JUMPIF:
            case LOP_JUMPIFNOT:
            case LOP_JUMPIFEQ:
            case LOP_JUMPIFLE:
            case LOP_JUMPIFLT:
            case LOP_JUMPIFNOTEQ:
            case LOP_JUMPIFNOTLE:
            case LOP_JUMPIFNOTLT:
            case LOP_FORNPREP:
            case LOP_FORNLOOP:
            case LOP_FORGPREP:
            case LOP_FORGPREP_NEXT:
            case LOP_FORGPREP_INEXT:
            case LOP_FORGLOOP:
                parseJump(op, getJumpTarget(insn, i));
                break;

            case LOP_ADD:
            case LOP_SUB:
            case LOP_MUL:
            case LOP_DIV:
            case LOP_MOD:
            case LOP_POW:
            case LOP_AND:
            case LOP_OR:
                addVmRegInput(node, LUAU_INSN_B(insn));
                addVmRegInput(node, LUAU_INSN_C(insn));
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_ADDK:
            case LOP_SUBK:
            case LOP_MULK:
            case LOP_DIVK:
            case LOP_MODK:
            case LOP_POWK:
            case LOP_ANDK:
            case LOP_ORK:
                addVmRegInput(node, LUAU_INSN_B(insn));
                addVmConstInput(node, LUAU_INSN_C(insn));
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_CONCAT:
            {
                LUAU_ASSERT(LUAU_INSN_B(insn) <= LUAU_INSN_C(insn));
                for (Reg param = LUAU_INSN_B(insn); param <= LUAU_INSN_C(insn); param++)
                    addVmRegInput(node, param);
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;
            }

            case LOP_NOT:
            case LOP_MINUS:
            case LOP_LENGTH:
                addVmRegInput(node, LUAU_INSN_B(insn));
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_NEWTABLE:
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_B(insn)));
                addImmInput(node, static_cast<int32_t>(aux));
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_DUPTABLE:
                addProducer(LUAU_INSN_A(insn), nodeOp);
                addVmConstInput(node, LUAU_INSN_D(insn));
                break;

            case LOP_SETLIST:
            {
                int count = LUAU_INSN_C(insn) - 1;
                addImmInput(node, static_cast<int32_t>(aux));
                addImmInput(node, static_cast<int32_t>(count));
                addVmRegInput(node, LUAU_INSN_A(insn));
                for (Reg param = 0; param < count; param++)
                    addVmRegInput(node, LUAU_INSN_B(insn) + param);
                if (count < 0)
                    for (auto inp : findProducersUpToTop(currentBlock, LUAU_INSN_B(insn)))
                        func.addUse(node, inp);
                break;
            }

            case LOP_FASTCALL:
                // Note that FASTCALL will read the actual call arguments, such as argument/result registers and counts, from the CALL instruction
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_A(insn)));
                // turn it in BcOp to CALL BcInst&.
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_C(insn)));
                break;

            case LOP_FASTCALL1:
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_A(insn)));
                addVmRegInput(node, LUAU_INSN_B(insn));
                // turn it in BcOp to CALL BcInst&.
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_C(insn)));
                break;

            case LOP_FASTCALL2:
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_A(insn)));
                addVmRegInput(node, LUAU_INSN_B(insn));
                addVmRegInput(node, aux & 0xFF);
                // turn it in BcOp to CALL BcInst&.
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_C(insn)));
                break;

            case LOP_FASTCALL2K:
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_A(insn)));
                addVmRegInput(node, LUAU_INSN_B(insn));
                addVmConstInput(node, aux);
                // turn it in BcOp to CALL BcInst&.
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_C(insn)));
                break;

            case LOP_FASTCALL3:
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_A(insn)));
                addVmRegInput(node, LUAU_INSN_B(insn));
                addVmRegInput(node, aux & 0xFF);
                addVmRegInput(node, (aux >> 8) & 0xFF);
                // turn it in BcOp to CALL BcInst&.
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_C(insn)));
                break;

            case LOP_GETVARARGS:
            {
                func.addUse(node, BcOp{BcOpKind::VmReg, LUAU_INSN_A(insn)});
                int count = LUAU_INSN_B(insn) - 1;
                addImmInput(node, static_cast<int32_t>(count));
                func.regs[nodeOp] = LUAU_INSN_A(insn);
                if (count < 0)
                {
                    BlockProducers& blockProducers = producers[currentBlock.index];
                    blockProducers.multiReturn = nodeOp;
                    blockProducers.multiReturnStart = LUAU_INSN_A(insn);
                    blockProducers.invalidAfter = 255;
                }
                else
                    for (int i = 0; i < count; i++)
                        addProducer(LUAU_INSN_A(insn) + i, func.addProj(nodeOp, i));
                break;
            }

            case LOP_DUPCLOSURE:
                addVmConstInput(node, LUAU_INSN_D(insn));
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_PREPVARARGS:
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_A(insn)));
                break;

            case LOP_LOADKX:
                addVmConstInput(node, aux);
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_JUMPX:
                LUAU_ASSERT(!"Shouldn't parse it directly");
                addJumpInput(node, getJumpTarget(insn, i));
                break;

            case LOP_COVERAGE:
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_E(insn)));
                break;

            case LOP_CAPTURE:
            {
                uint8_t captureType = LUAU_INSN_A(insn);
                addImmInput(node, static_cast<int32_t>(captureType));
                if (captureType == LCT_VAL || captureType == LCT_REF)
                    addVmRegInput(node, LUAU_INSN_B(insn));
                else
                    addUpvalInput(node, LUAU_INSN_B(insn));
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_C(insn)));
                break;
            }

            case LOP_SUBRK:
            case LOP_DIVRK:
                addVmConstInput(node, LUAU_INSN_B(insn));
                addVmRegInput(node, LUAU_INSN_C(insn));
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_IDIV:
                addVmRegInput(node, LUAU_INSN_B(insn));
                addVmRegInput(node, LUAU_INSN_C(insn));
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_IDIVK:
                addVmRegInput(node, LUAU_INSN_B(insn));
                addVmConstInput(node, LUAU_INSN_C(insn));
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_CMPPROTO:
                addVmRegInput(node, LUAU_INSN_A(insn));
                addImmInput(node, static_cast<int32_t>(aux));
                addJumpInput(node, getJumpTarget(insn, i));
                break;

            case LOP_NEWCLASSMEMBER:
                LUAU_ASSERT(FFlag::DebugLuauUserDefinedClasses);
                addVmRegInput(node, LUAU_INSN_A(insn));
                addVmRegInput(node, LUAU_INSN_C(insn));
                addVmConstInput(node, aux);
                break;

            case LOP_FASTPCALL:
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_A(insn)));
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_B(insn)));
                // turn it in BcOp to CALL BcInst&.
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_C(insn)));
                break;
            case LOP_NEWCLASS:
                LUAU_ASSERT(FFlag::DebugLuauUserDefinedClasses);

                if (int reg = LUAU_INSN_B(insn); reg != 0xff)
                    addVmRegInput(node, LUAU_INSN_B(insn));
                else
                    addEmptyInput(node);

                addImmInput(node, static_cast<uint32_t>(LUAU_INSN_C(insn)));
                addVmConstInput(node, aux);
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_CONSTRUCT:
                LUAU_ASSERT(aux < func.feedbackSlots.size());
                LUAU_ASSERT(func.feedbackSlots[aux].kind == LFT_CONSTRUCT);
                addVmRegInput(node, LUAU_INSN_B(insn));
                addImmInput(node, static_cast<int32_t>(aux));
                addProducer(LUAU_INSN_A(insn), nodeOp);
                break;

            case LOP_FINCONSTRUCT:
                addVmRegInput(node, LUAU_INSN_A(insn));
                addImmInput(node, static_cast<int32_t>(LUAU_INSN_C(insn)));
                break;

            case LOP__COUNT:
                LUAU_UNREACHABLE();
            }

            i += opLength;
            if (blockByPC.count(i) > 0)
            {
                // currentBlock is fully emitted: release it so its successors can seal once all their
                // predecessors are emitted (a loop header seals here, after its back-edge source)
                finalizeBlock(currentBlock);
                currentBlock = blockByPC[i];
            }
        }

        finalizeBlock(currentBlock);
        sealAllRemaining(); // seal any block whose predecessors were never all emitted

        return true;
    }
};

} // namespace Bytecode
} // namespace Luau
