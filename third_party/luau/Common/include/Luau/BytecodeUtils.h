// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/Bytecode.h"

#include <stdint.h>

namespace Luau
{

inline int getOpLength(LuauOpcode op)
{
    switch (op)
    {
    case LOP_GETGLOBAL:
    case LOP_SETGLOBAL:
    case LOP_GETIMPORT:
    case LOP_GETTABLEKS:
    case LOP_SETTABLEKS:
    case LOP_NAMECALL:
    case LOP_JUMPIFEQ:
    case LOP_JUMPIFLE:
    case LOP_JUMPIFLT:
    case LOP_JUMPIFNOTEQ:
    case LOP_JUMPIFNOTLE:
    case LOP_JUMPIFNOTLT:
    case LOP_NEWTABLE:
    case LOP_SETLIST:
    case LOP_FORGLOOP:
    case LOP_LOADKX:
    case LOP_FASTCALL2:
    case LOP_FASTCALL2K:
    case LOP_FASTCALL3:
    case LOP_JUMPXEQKNIL:
    case LOP_JUMPXEQKB:
    case LOP_JUMPXEQKN:
    case LOP_JUMPXEQKS:
    case LOP_GETUDATAKS:
    case LOP_SETUDATAKS:
    case LOP_NAMECALLUDATA:
    case LOP_NEWCLASSMEMBER:
    case LOP_CALLFB:
    case LOP_CMPPROTO:
    case LOP_NEWCLASS:
    case LOP_CONSTRUCT:
        return 2;

    default:
        return 1;
    }
}

inline bool isFastCall(LuauOpcode op)
{
    switch (op)
    {
    case LOP_FASTCALL:
    case LOP_FASTCALL1:
    case LOP_FASTCALL2:
    case LOP_FASTCALL2K:
    case LOP_FASTCALL3:
    case LOP_FASTPCALL:
    case LOP_FINCONSTRUCT:
        return true;

    default:
        return false;
    }
}

inline bool isJumpD(LuauOpcode op)
{
    switch (op)
    {
    case LOP_JUMP:
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
    case LOP_FORGLOOP:
    case LOP_FORGPREP_INEXT:
    case LOP_FORGPREP_NEXT:
    case LOP_JUMPBACK:
    case LOP_JUMPXEQKNIL:
    case LOP_JUMPXEQKB:
    case LOP_JUMPXEQKN:
    case LOP_JUMPXEQKS:
    case LOP_CMPPROTO:
        return true;

    default:
        return false;
    }
}

inline bool isSkipC(LuauOpcode op)
{
    switch (op)
    {
    case LOP_LOADB:
        return true;

    default:
        return false;
    }
}

inline int getJumpTarget(uint32_t insn, uint32_t pc)
{
    LuauOpcode op = LuauOpcode(LUAU_INSN_OP(insn));

    if (isJumpD(op))
        return int(pc + LUAU_INSN_D(insn) + 1);
    else if (isFastCall(op))
        return int(pc + LUAU_INSN_C(insn) + 2);
    else if (isSkipC(op) && LUAU_INSN_C(insn))
        return int(pc + LUAU_INSN_C(insn) + 1);
    else if (op == LOP_JUMPX)
        return int(pc + LUAU_INSN_E(insn) + 1);
    else
        return -1;
}

inline bool isFallthrough(LuauOpcode op)
{
    switch (op)
    {
    case LOP_RETURN:
    case LOP_JUMP:
    case LOP_JUMPBACK:
    case LOP_JUMPX:
    case LOP_FORGPREP:
    case LOP_FORGPREP_NEXT:
    case LOP_FORGPREP_INEXT:
        return false;
    default:
        return true;
    }
}

inline bool isLoopJump(LuauOpcode op)
{
    switch (op)
    {
    case LOP_JUMPBACK:
    case LOP_FORGLOOP:
    case LOP_FORNLOOP:
        return true;
    default:
        return false;
    }
}

} // namespace Luau
