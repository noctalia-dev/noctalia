// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/BytecodeGraph.h"
#include "Luau/BytecodeUtils.h"
#include "Luau/BytecodeOps.h"

namespace Luau
{
namespace Bytecode
{

template<typename VmConst>
struct BytecodeGraphSerializer
{
    struct JumpInfo
    {
        LuauOpcode op;
        uint32_t instructionPC;
        BcOp targetBlock;
    };
    using Jumps = std::vector<JumpInfo>;

    BytecodeBuilder& bcb;
    BcFunction<VmConst>& func;
    Jumps jumps;
    bool error = false;

    BytecodeGraphSerializer(BytecodeBuilder& bcb, BcFunction<VmConst>& func)
        : bcb(bcb)
        , func(func)
    {
    }

    std::vector<BcOp> reschedule()
    {
        std::vector<BcOp> sortedBlocks;
        sortedBlocks.reserve(func.blocks.size());
        for (uint32_t i = 0; i < func.blocks.size(); i++)
            if ((func.blocks[i].flags & BcBlockFlag::Dead) == 0)
                sortedBlocks.push_back(BcOp{BcOpKind::Block, i});

        std::sort(
            sortedBlocks.begin(),
            sortedBlocks.end(),
            [&](BcOp opA, BcOp opB)
            {
                const BcBlock& a = func.blockOp(opA);
                const BcBlock& b = func.blockOp(opB);

                if (a.sortkey == b.sortkey)
                    return a.chainkey < b.chainkey;

                return a.sortkey < b.sortkey;
            }
        );

        LUAU_ASSERT(sortedBlocks.back() == func.exitBlock);
        sortedBlocks.pop_back();

        return sortedBlocks;
    }

    uint32_t getRegisterRaw(BcOp op)
    {
        switch (op.kind)
        {
        case BcOpKind::Phi:
        {
            BcPhi& phi = func.phiOp(op);
            LUAU_ASSERT(phi.ops.size() > 0);
            // Parser-built phis record their register at creation in `makePhi`
            // Loop-carried phis form operand cycles, as a nested loop's inner and outer accumulator phis are mutual operands, so recursing through
            // phi.ops to derive the register would not terminate, so we should use the recorded register instead
            // Return-value merges, inserted by the inliner, have no recorded register but are acyclic, so they resolve through their first operand
            if (auto it = func.regs.find(op); it != func.regs.end())
                return it->second;
            LUAU_ASSERT(phi.ops[0] != op);
            return getRegister(phi.ops[0]);
        }
        case BcOpKind::Inst:
        {
            auto it = func.regs.find(op);
            LUAU_ASSERT(it != func.regs.end());
            return it->second;
        }
        case BcOpKind::Proj:
        {
            BcProj& proj = func.projOp(op);
            Reg base = getRegister(proj.op);
            return base + proj.index;
        }
        case BcOpKind::VmReg:
            return op.index;
        default:
            LUAU_UNREACHABLE();
        }
        LUAU_UNREACHABLE();
        return 0;
    }

    uint8_t getRegister(BcOp op)
    {
        uint32_t reg = getRegisterRaw(op);

        if (reg >= kInvalidReg)
        {
            LUAU_ASSERT(!"register reference overflow");
            error = true;
        }

        if (reg >= func.maxstacksize)
        {
            LUAU_ASSERT(!"register overflows the function stack");
            error = true;
        }

        return uint8_t(reg);
    }

    uint8_t getRegInput(BcInst& insn, uint8_t index)
    {
        LUAU_ASSERT(index < insn.ops.size());
        return getRegister(insn.ops[index]);
    }

    uint8_t getRegInputForRange(BcInst& insn, uint8_t index, int count)
    {
        LUAU_ASSERT(index < insn.ops.size());

        uint32_t reg = getRegisterRaw(insn.ops[index]);

        if (count < -1 || count >= 255)
        {
            LUAU_ASSERT(!"register count overflow");
            error = true;
        }

        int range = count < 0 ? 0 : count;

        if (reg >= kInvalidReg)
        {
            LUAU_ASSERT(!"register reference overflow");
            error = true;
        }

        if (reg + range > func.maxstacksize)
        {
            LUAU_ASSERT(!"register overflows the function stack");
            error = true;
        }

        return uint8_t(reg);
    }

    BcImm& getImm(BcInst& insn, uint8_t index)
    {
        LUAU_ASSERT(index < insn.ops.size());
        BcOp inp = insn.ops[index];
        LUAU_ASSERT(inp.kind == BcOpKind::Imm);
        return func.immOp(inp);
    }

    int32_t getImmInt(BcInst& insn, uint8_t index)
    {
        BcImm& imm = getImm(insn, index);
        LUAU_ASSERT(imm.kind == BcImmKind::Int);
        return imm.valueInt;
    }

    uint8_t getImmIntAsUnsignedABC(BcInst& insn, uint8_t index, int8_t bias)
    {
        BcImm& imm = getImm(insn, index);
        LUAU_ASSERT(imm.kind == BcImmKind::Int);

        int64_t result = int64_t(imm.valueInt) + bias;

        if (int64_t(uint8_t(result)) != result)
            error = true;

        return uint8_t(result);
    }

    int16_t getImmIntAsSignedD(BcInst& insn, uint8_t index)
    {
        BcImm& imm = getImm(insn, index);
        LUAU_ASSERT(imm.kind == BcImmKind::Int);

        if (int32_t(int16_t(imm.valueInt)) != imm.valueInt)
            error = true;

        return int16_t(imm.valueInt);
    }

    bool getImmBool(BcInst& insn, uint8_t index)
    {
        BcImm& imm = getImm(insn, index);
        LUAU_ASSERT(imm.kind == BcImmKind::Boolean);
        return imm.valueBoolean;
    }

    uint32_t getImmImport(BcInst& insn, uint8_t index)
    {
        BcImm& imm = getImm(insn, index);
        LUAU_ASSERT(imm.kind == BcImmKind::Import);
        return imm.valueImport;
    }

    virtual uint32_t getVmConstInputRaw(BcInst& insn, uint8_t index)
    {
        LUAU_ASSERT(index < insn.ops.size());
        BcOp inp = insn.ops[index];
        LUAU_ASSERT(inp.kind == BcOpKind::VmConst);
        LUAU_ASSERT(inp.index < func.constants.size());
        return inp.index;
    }

    uint8_t getVmConstInputABC(BcInst& insn, uint8_t index)
    {
        uint32_t cid = getVmConstInputRaw(insn, index);

        if (cid > 0xff)
            error = true;

        return uint8_t(cid);
    }

    uint16_t getVmConstInputD(BcInst& insn, uint8_t index)
    {
        uint32_t cid = getVmConstInputRaw(insn, index);

        if (cid > 0x7fff)
            error = true;

        return uint16_t(cid);
    }

    uint32_t getVmConstInputAux16(BcInst& insn, uint8_t index)
    {
        uint32_t cid = getVmConstInputRaw(insn, index);

        if (cid > 0xffff)
            error = true;

        return cid;
    }

    uint32_t getVmConstInputAux(BcInst& insn, uint8_t index)
    {
        uint32_t cid = getVmConstInputRaw(insn, index);

        if (cid >= kMaxConstantCount)
            error = true;

        return cid;
    }

    uint8_t getUpvalInput(BcInst& insn, uint8_t index)
    {
        LUAU_ASSERT(index < insn.ops.size());
        BcOp inp = insn.ops[index];
        LUAU_ASSERT(inp.kind == BcOpKind::VmUpvalue);

        if (inp.index >= func.nups)
        {
            LUAU_ASSERT(!"upvalue reference overflows the function upvalue count");
            error = true;
        }

        if (inp.index >= kMaxUpvalueCount)
            error = true;

        LUAU_ASSERT(inp.index < func.nups);
        return uint8_t(inp.index);
    }

    uint16_t getProtoInput(BcInst& insn, uint8_t index)
    {
        LUAU_ASSERT(index < insn.ops.size());
        BcOp inp = insn.ops[index];
        LUAU_ASSERT(inp.kind == BcOpKind::VmProto);

        if (inp.index >= kMaxClosureCount)
            error = true;

        return uint16_t(inp.index);
    }

    void recordJump(BcInst& insn, uint8_t index)
    {
        LUAU_ASSERT(index < insn.ops.size());
        BcOp inp = insn.ops[index];
        LUAU_ASSERT(inp.kind == BcOpKind::Block);
        jumps.push_back({insn.op, static_cast<uint32_t>(bcb.getInstructionCount()), inp});
    }

    void patchJump(JumpInfo& jump)
    {
        BcBlock& target = func.blockOp(jump.targetBlock);
        LUAU_ASSERT(target.startpc != kBlockNoStartPc);

        if (isJumpD(jump.op))
        {
            if (!bcb.patchJumpD(jump.instructionPC, target.startpc))
                error = true;
        }
        else if (isSkipC(jump.op))
        {
            if (!bcb.patchSkipC(jump.instructionPC, target.startpc))
                error = true;
        }
    }

    void emitInstruction(BcOp insnOp)
    {
        BcInst& insn = func.instOp(insnOp);
        bcb.setDebugLine(insn.line);
        switch (insn.op)
        {
        case LOP_NOP:
        case LOP_BREAK:
        case LOP_NATIVECALL:
            bcb.emitABC(insn.op, 0, 0, 0);
            break;

        case LOP_LOADNIL:
            bcb.emitABC(LOP_LOADNIL, getRegister(insnOp), 0, 0);
            break;

        case LOP_LOADB:
        {
            if (insn.ops.size() > 1)
                recordJump(insn, 1);
            bcb.emitABC(LOP_LOADB, getRegister(insnOp), getImmBool(insn, 0), 0);
            break;
        }

        case LOP_LOADN:
            bcb.emitAD(LOP_LOADN, getRegister(insnOp), getImmIntAsSignedD(insn, 0));
            break;

        case LOP_LOADK:
            bcb.emitAD(LOP_LOADK, getRegister(insnOp), getVmConstInputD(insn, 0));
            break;

        case LOP_MOVE:
            bcb.emitABC(LOP_MOVE, getRegister(insnOp), getRegInput(insn, 0), 0);
            break;

        case LOP_GETGLOBAL:
            bcb.emitABC(LOP_GETGLOBAL, getRegister(insnOp), 0, getImmInt(insn, 0));
            bcb.emitAux(getVmConstInputAux(insn, 1));
            break;

        case LOP_SETGLOBAL:
            bcb.emitABC(LOP_SETGLOBAL, getRegInput(insn, 0), 0, getImmInt(insn, 1));
            bcb.emitAux(getVmConstInputAux(insn, 2));
            break;

        case LOP_GETUPVAL:
            bcb.emitABC(LOP_GETUPVAL, getRegister(insnOp), getUpvalInput(insn, 0), 0);
            break;

        case LOP_SETUPVAL:
            bcb.emitABC(LOP_SETUPVAL, getRegInput(insn, 0), getUpvalInput(insn, 1), 0);
            break;

        case LOP_CLOSEUPVALS:
            bcb.emitABC(LOP_CLOSEUPVALS, getRegInput(insn, 0), 0, 0);
            break;

        case LOP_GETIMPORT:
        {
            bcb.emitAD(LOP_GETIMPORT, getRegister(insnOp), getVmConstInputD(insn, 0));
            uint32_t componentsCount = getImmInt(insn, 1);
            LUAU_ASSERT(componentsCount > 0 && componentsCount <= 3);
            LUAU_ASSERT(insn.ops.size() - 2 == componentsCount);
            uint32_t aux = componentsCount << 30;
            for (uint32_t i = 0; i < componentsCount; i++)
            {
                uint32_t componentId = getVmConstInputRaw(insn, 2 + i);
                if (componentId > 0x3FF)
                    error = true;
                aux |= componentId << (20 - 10 * i);
            }
            bcb.emitAux(aux);
            break;
        }

        case LOP_GETTABLE:
            bcb.emitABC(LOP_GETTABLE, getRegister(insnOp), getRegInput(insn, 0), getRegInput(insn, 1));
            break;

        case LOP_SETTABLE:
            bcb.emitABC(LOP_SETTABLE, getRegInput(insn, 0), getRegInput(insn, 1), getRegInput(insn, 2));
            break;

        case LOP_GETUDATAKS:
        case LOP_GETTABLEKS:
            bcb.emitABC(insn.op, getRegister(insnOp), getRegInput(insn, 0), getImmInt(insn, 1));

            if (insn.op == LOP_GETUDATAKS)
                bcb.emitAux(getVmConstInputAux16(insn, 2) | static_cast<uint32_t>(getImmInt(insn, 3)) << 16);
            else
                bcb.emitAux(getVmConstInputAux(insn, 2));
            break;

        case LOP_SETUDATAKS:
        case LOP_SETTABLEKS:
            bcb.emitABC(insn.op, getRegInput(insn, 0), getRegInput(insn, 1), getImmInt(insn, 2));

            if (insn.op == LOP_SETUDATAKS)
                bcb.emitAux(getVmConstInputAux16(insn, 3) | static_cast<uint32_t>(getImmInt(insn, 4)) << 16);
            else
                bcb.emitAux(getVmConstInputAux(insn, 3));
            break;

        case LOP_GETTABLEN:
            bcb.emitABC(LOP_GETTABLEN, getRegister(insnOp), getRegInput(insn, 0), getImmIntAsUnsignedABC(insn, 1, -1));
            break;

        case LOP_SETTABLEN:
            bcb.emitABC(LOP_SETTABLEN, getRegInput(insn, 0), getRegInput(insn, 1), getImmIntAsUnsignedABC(insn, 2, -1));
            break;

        case LOP_NEWCLOSURE:
            bcb.emitAD(LOP_NEWCLOSURE, getRegister(insnOp), getProtoInput(insn, 0));
            break;

        case LOP_NAMECALLUDATA:
        case LOP_NAMECALL:
            bcb.emitABC(insn.op, getRegister(insnOp), getRegInput(insn, 0), getImmInt(insn, 1));

            if (insn.op == LOP_NAMECALLUDATA)
                bcb.emitAux(getVmConstInputAux16(insn, 2) | static_cast<uint32_t>(getImmInt(insn, 3)) << 16);
            else
                bcb.emitAux(getVmConstInputAux(insn, 2));
            break;

        case LOP_CALL:
            bcb.emitABC(LOP_CALL, getRegInput(insn, 2), getImmInt(insn, 0) + 1, getImmInt(insn, 1) + 1);
            break;

        case LOP_CALLFB:
            bcb.emitABC(LOP_CALLFB, getRegInput(insn, 3), getImmInt(insn, 0) + 1, getImmInt(insn, 1) + 1);
            bcb.emitAux(getImmInt(insn, 2));
            break;

        case LOP_RETURN:
        {
            LUAU_ASSERT(insn.ops.size() > 1);
            bcb.emitABC(LOP_RETURN, getRegInputForRange(insn, 1, getImmInt(insn, 0)), getImmInt(insn, 0) + 1, 0);
            break;
        }

        case LOP_JUMP:
            recordJump(insn, 0);
            bcb.emitAD(LOP_JUMP, 0, 0);
            break;

        case LOP_JUMPBACK:
            recordJump(insn, 0);
            bcb.emitAD(LOP_JUMPBACK, 0, 0);
            break;

        case LOP_JUMPIFNOT:
        case LOP_JUMPIF:
            recordJump(insn, 1);
            bcb.emitAD(insn.op, getRegInput(insn, 0), 0);
            break;

        case LOP_JUMPIFEQ:
        case LOP_JUMPIFLE:
        case LOP_JUMPIFLT:
        case LOP_JUMPIFNOTEQ:
        case LOP_JUMPIFNOTLE:
        case LOP_JUMPIFNOTLT:
            recordJump(insn, 2);
            bcb.emitAD(insn.op, getRegInput(insn, 0), 0);
            bcb.emitAux(getRegInput(insn, 1));
            break;

        case LOP_ADD:
        case LOP_SUB:
        case LOP_MUL:
        case LOP_DIV:
        case LOP_MOD:
        case LOP_POW:
        case LOP_AND:
        case LOP_OR:
            bcb.emitABC(insn.op, getRegister(insnOp), getRegInput(insn, 0), getRegInput(insn, 1));
            break;

        case LOP_ADDK:
        case LOP_SUBK:
        case LOP_MULK:
        case LOP_DIVK:
        case LOP_MODK:
        case LOP_POWK:
        case LOP_ANDK:
        case LOP_ORK:
            bcb.emitABC(insn.op, getRegister(insnOp), getRegInput(insn, 0), getVmConstInputABC(insn, 1));
            break;

        case LOP_CONCAT:
            LUAU_ASSERT(insn.ops.size() > 0);
            bcb.emitABC(LOP_CONCAT, getRegister(insnOp), getRegInput(insn, 0), getRegInput(insn, insn.ops.size() - 1));
            break;

        case LOP_NOT:
        case LOP_MINUS:
        case LOP_LENGTH:
            bcb.emitABC(insn.op, getRegister(insnOp), getRegInput(insn, 0), 0);
            break;

        case LOP_NEWTABLE:
            bcb.emitABC(LOP_NEWTABLE, getRegister(insnOp), getImmInt(insn, 0), 0);
            bcb.emitAux(getImmInt(insn, 1));
            break;

        case LOP_DUPTABLE:
            bcb.emitAD(LOP_DUPTABLE, getRegister(insnOp), getVmConstInputD(insn, 0));
            break;

        case LOP_SETLIST:
        {
            LUAU_ASSERT(insn.ops.size() > 2);
            int32_t count = getImmInt(insn, 1);
            LUAU_ASSERT(count < 255);
            Reg startReg = getRegInput(insn, 2);
            if (count != 0)
            {
                LUAU_ASSERT(insn.ops.size() > 3);
                startReg = getRegInput(insn, 3);
            }
            bcb.emitABC(LOP_SETLIST, getRegInput(insn, 2), startReg, static_cast<uint8_t>(count + 1));
            bcb.emitAux(getImmInt(insn, 0));
            break;
        }

        case LOP_FORNPREP:
            recordJump(insn, 3);
            bcb.emitAD(LOP_FORNPREP, getRegInput(insn, 0), 0);
            break;

        case LOP_FORNLOOP:
            recordJump(insn, 3);
            bcb.emitAD(LOP_FORNLOOP, getRegInput(insn, 0), 0);
            break;

        case LOP_FORGPREP:
        case LOP_FORGPREP_NEXT:
        case LOP_FORGPREP_INEXT:
            recordJump(insn, 3);
            bcb.emitAD(insn.op, getRegInput(insn, 0), 0);
            break;

        case LOP_FORGLOOP:
            recordJump(insn, 5);
            bcb.emitAD(LOP_FORGLOOP, getRegInput(insn, 0), 0);
            bcb.emitAux(static_cast<uint32_t>(getImmBool(insn, 3)) << 31 | getImmInt(insn, 4));
            break;

        case LOP_FASTCALL:
            bcb.emitABC(LOP_FASTCALL, getImmInt(insn, 0), 0, getImmInt(insn, 1));
            break;

        case LOP_FASTCALL1:
            bcb.emitABC(LOP_FASTCALL1, getImmInt(insn, 0), getRegInput(insn, 1), getImmInt(insn, 2));
            break;

        case LOP_FASTCALL2:
            bcb.emitABC(LOP_FASTCALL2, getImmInt(insn, 0), getRegInput(insn, 1), getImmInt(insn, 3));
            bcb.emitAux(getRegInput(insn, 2));
            break;

        case LOP_FASTCALL2K:
            bcb.emitABC(LOP_FASTCALL2K, getImmInt(insn, 0), getRegInput(insn, 1), getImmInt(insn, 3));
            bcb.emitAux(getVmConstInputAux(insn, 2));
            break;

        case LOP_FASTCALL3:
            bcb.emitABC(LOP_FASTCALL3, getImmInt(insn, 0), getRegInput(insn, 1), getImmInt(insn, 4));
            bcb.emitAux(getRegInput(insn, 2) | static_cast<uint32_t>(getRegInput(insn, 3)) << 8);
            break;

        case LOP_GETVARARGS:
            LUAU_ASSERT(insn.ops.size() == 2 && insn.ops[0].kind == BcOpKind::VmReg);
            bcb.emitABC(LOP_GETVARARGS, getRegInputForRange(insn, 0, getImmInt(insn, 1)), getImmInt(insn, 1) + 1, 0);
            break;

        case LOP_DUPCLOSURE:
            bcb.emitAD(LOP_DUPCLOSURE, getRegister(insnOp), getVmConstInputD(insn, 0));
            break;

        case LOP_PREPVARARGS:
            bcb.emitAD(LOP_PREPVARARGS, getImmInt(insn, 0), 0);
            break;

        case LOP_LOADKX:
            bcb.emitAD(LOP_LOADKX, getRegister(insnOp), 0);
            bcb.emitAux(getVmConstInputAux(insn, 0));
            break;

        case LOP_JUMPX:
            recordJump(insn, 0);
            bcb.emitE(LOP_JUMPX, 0);
            break;

        case LOP_COVERAGE:
            bcb.emitE(LOP_COVERAGE, getImmInt(insn, 0));
            break;

        case LOP_CAPTURE:
        {
            uint8_t captureType = getImmInt(insn, 0);
            if (captureType == LCT_VAL || captureType == LCT_REF)
                bcb.emitABC(LOP_CAPTURE, captureType, getRegInput(insn, 1), getImmInt(insn, 2));
            else
                bcb.emitABC(LOP_CAPTURE, captureType, getUpvalInput(insn, 1), getImmInt(insn, 2));
            break;
        }

        case LOP_SUBRK:
        case LOP_DIVRK:
            bcb.emitABC(insn.op, getRegister(insnOp), getVmConstInputABC(insn, 0), getRegInput(insn, 1));
            break;

        case LOP_JUMPXEQKNIL:
            recordJump(insn, 2);
            bcb.emitAD(LOP_JUMPXEQKNIL, getRegInput(insn, 0), 0);
            bcb.emitAux(static_cast<uint32_t>(getImmBool(insn, 1)) << 31);
            break;

        case LOP_JUMPXEQKB:
            recordJump(insn, 2);
            bcb.emitAD(LOP_JUMPXEQKB, getRegInput(insn, 0), 0);
            bcb.emitAux(static_cast<uint32_t>(getImmBool(insn, 1)) << 31 | static_cast<uint32_t>(getImmBool(insn, 3)));
            break;

        case LOP_JUMPXEQKN:
        case LOP_JUMPXEQKS:
            recordJump(insn, 2);
            bcb.emitAD(insn.op, getRegInput(insn, 0), 0);
            bcb.emitAux(static_cast<uint32_t>(getImmBool(insn, 1)) << 31 | getVmConstInputAux(insn, 3));
            break;

        case LOP_IDIV:
            bcb.emitABC(LOP_IDIV, getRegister(insnOp), getRegInput(insn, 0), getRegInput(insn, 1));
            break;

        case LOP_IDIVK:
            bcb.emitABC(LOP_IDIVK, getRegister(insnOp), getRegInput(insn, 0), getVmConstInputABC(insn, 1));
            break;

        case LOP_NEWCLASSMEMBER:
            LUAU_ASSERT(FFlag::DebugLuauUserDefinedClasses);
            bcb.emitABC(LOP_NEWCLASSMEMBER, getRegInput(insn, 0), 0, getRegInput(insn, 1));
            bcb.emitAux(getVmConstInputAux(insn, 2));
            break;

        case LOP_CMPPROTO:
            recordJump(insn, 2);
            bcb.emitAD(LOP_CMPPROTO, getRegInput(insn, 0), 0);
            bcb.emitAux(getImmInt(insn, 1));
            break;

        case LOP_FASTPCALL:
            bcb.emitABC(LOP_FASTPCALL, getImmInt(insn, 0), getImmInt(insn, 1), getImmInt(insn, 2));
            break;

        case LOP_NEWCLASS:
            LUAU_ASSERT(FFlag::DebugLuauUserDefinedClasses);

            if (insn.ops[0].kind != BcOpKind::None)
                bcb.emitABC(LOP_NEWCLASS, getRegister(insnOp), getRegInput(insn, 0), getImmImport(insn, 1));
            else
                bcb.emitABC(LOP_NEWCLASS, getRegister(insnOp), 0xff, getImmImport(insn, 1));

            bcb.emitAux(getVmConstInputAux(insn, 2));
            break;

        case LOP_CONSTRUCT:
            bcb.emitABC(LOP_CONSTRUCT, getRegister(insnOp), getRegInput(insn, 0), 0);
            bcb.emitAux(getImmInt(insn, 1));
            break;

        case LOP_FINCONSTRUCT:
            bcb.emitABC(LOP_FINCONSTRUCT, getRegInput(insn, 0), 0, getImmInt(insn, 1));
            break;

        case LOP__COUNT:
            LUAU_UNREACHABLE();
        }
    }

    std::optional<BcOp> getFallthrough(BcBlock& block)
    {
        for (auto [ctrl, target] : block.successors)
            if (ctrl == BcBlockEdgeKind::Fallthrough)
                return {target};
        return {};
    }

    std::vector<uint32_t> emitBytecode()
    {
        std::vector<BcOp> schedule = reschedule();
        std::vector<uint32_t> insnsPC;
        insnsPC.resize(func.instructions.size(), ~0u);

        for (size_t i = 0; i < schedule.size(); i++)
        {
            BcOp blockOp = schedule[i];
            BcBlock& block = func.blockOp(blockOp);
            std::optional<BcOp> fallthrough = getFallthrough(block);
            if (fallthrough && *fallthrough != func.exitBlock && !(func.blockOp(*fallthrough).flags & BcBlockFlag::Dead) &&
                (i + 1 >= schedule.size() || *fallthrough != schedule[i + 1]))
            {
                BcJump jump = BcJump<VmConst>::create(func);
                jump.setTarget(*fallthrough);
                jump.appendTo(blockOp);
                insnsPC.resize(func.instructions.size());
            }
            block.startpc = bcb.getDebugPC();
            for (BcOp op : block.ops)
            {
                LUAU_ASSERT(op.kind == BcOpKind::Inst);
                insnsPC[op.index] = bcb.getDebugPC();
                emitInstruction(op);
            }
        }

        for (auto& jump : jumps)
            patchJump(jump);

        // Serialization failed
        if (error)
            return {};

        return insnsPC;
    }
};

} // namespace Bytecode
} // namespace Luau
