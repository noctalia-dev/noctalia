// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

// clang-format off

// This header contains the bytecode definition for Luau interpreter
// Creating the bytecode is outside the scope of this file and is handled by bytecode builder (BytecodeBuilder.h) and bytecode compiler (Compiler.h)
// Note that ALL enums declared in this file are order-sensitive since the values are baked into bytecode that needs to be processed by legacy clients.

// # Bytecode definitions
// Bytecode instructions are using "word code" - each instruction is one or many 32-bit words.
// The first word in the instruction is always the instruction header, and *must* contain the opcode (enum below) in the least significant byte.
//
// Instruction word can be encoded using one of the following encodings:
//     ABC - least-significant byte for the opcode, followed by three bytes, A, B and C; each byte declares a register index, small index into some other table or an unsigned integral value
//     AD - least-significant byte for the opcode, followed by A byte, followed by D half-word (16-bit integer). D is a signed integer that commonly specifies constant table index or jump offset
//     E - least-significant byte for the opcode, followed by E (24-bit integer). E is a signed integer that commonly specifies a jump offset
//
// Instruction word is sometimes followed by one extra word, indicated as AUX - this is just a 32-bit word and is decoded according to the specification for each opcode.
// For each opcode the encoding is *static* - that is, based on the opcode you know apriori how large the instruction is, with the exception of NEWCLOSURE

// # Bytecode indices
// Bytecode instructions commonly refer to integer values that define offsets or indices for various entities. For each type, there's a maximum encodable value.
// Note that in some cases, the compiler will set a lower limit than the maximum encodable value is to prevent fragile code into bumping against the limits whenever we change the compilation details.
// Additionally, in some specific instructions such as ANDK, the limit on the encoded value is smaller; this means that if a value is larger, a different instruction must be selected.
//
// Registers: 0-254. Registers refer to the values on the function's stack frame, including arguments.
// Upvalues: 0-199. Upvalues refer to the values stored in the closure object.
// Constants: 0-2^23-1. Constants are stored in a table allocated with each proto; to allow for future bytecode tweaks the encodable value is limited to 23 bits.
// Closures: 0-2^15-1. Closures are created from child protos via a child index; the limit is for the number of closures immediately referenced in each function.
// Jumps: -2^23..2^23. Jump offsets are specified in word increments, so jumping over an instruction may sometimes require an offset of 2 or more. Note that for jump instructions with AUX, the AUX word is included as part of the jump offset.

// # Bytecode versions
// Bytecode serialized format embeds a version number, that dictates both the serialized form as well as the allowed instructions. As long as the bytecode version falls into supported
// range (indicated by LBC_BYTECODE_MIN / LBC_BYTECODE_MAX) and was produced by Luau compiler, it should load and execute correctly.
//
// Note that Luau runtime doesn't provide indefinite bytecode compatibility: support for older versions gets removed over time. As such, bytecode isn't a durable storage format and it's expected
// that Luau users can recompile bytecode from source on Luau version upgrades if necessary.

// # Bytecode version history
//
// Note: due to limitations of the versioning scheme, some bytecode blobs that carry version 2 are using features from version 3. Starting from version 3, version should be sufficient to indicate bytecode compatibility.
//
// Version 1: Baseline version for the open-source release. Supported until 0.521.
// Version 2: Adds Proto::linedefined. Supported until 0.544.
// Version 3: Adds FORGPREP/JUMPXEQK* and enhances AUX encoding for FORGLOOP. Removes FORGLOOP_NEXT/INEXT and JUMPIFEQK/JUMPIFNOTEQK. Currently supported.
// Version 4: Adds Proto::flags, typeinfo, and floor division opcodes IDIV/IDIVK. Currently supported.
// Version 5: Adds SUBRK/DIVRK and vector constants. Currently supported.
// Version 6: Adds FASTCALL3. Currently supported.
// Version 7: Adds LBC_CONSTANT_TABLE_WITH_CONSTANTS for DUPTABLE with pre-filled constant values. Currently supported.
// Version 8: Adds LBC_CONSTANT_INTEGER for 64-bit integer constants. Currently supported.
// Version 9: Adds atom-based userdata field access acceleration. Currently supported.
// Version 10: Adds LBC_CONSTANT_CLASS_SHAPE and NEWCLASSMEMBER for use with Luau Classes. Experimental.
// Version 11: Adds CALLFB, CMPPROTO and feedback vector description. Experimental.
// Version 12: Adds cost function serialized for proto and prepend each proto with size in bytes. Experimental.
// Version 13: Adds support for double-precision vector constants. Experimental.
// Version 14: Adds FASTPCALL. Currently supported.

// WIP Versions: Used for in-progress features that might require multiple changes to bytecode. Since these versions are higher than the non-WIP versions, they are responsible for maintaining compatibility with them. For example, tests exercising WIP bytecode versions may need to enable flags for unreleased but non-WIP bytecode versions.
// Version 100: Adds NEWCLASS for use with Luau Classes. Future class-related bytecode changes should go in this version before release. Experimental.

// # Bytecode type information history
// Version 1: (from bytecode version 4) Type information for function signature. Currently supported.
// Version 2: (from bytecode version 4) Type information for arguments, upvalues, locals and some temporaries. Currently supported.
// Version 3: (from bytecode version 5) Type information for userdata type names and their index mapping. Currently supported.

// Bytecode opcode, part of the instruction header
enum LuauOpcode
{
    // NOP: noop
    LOP_NOP,

    // BREAK: debugger break
    LOP_BREAK,

    // LOADNIL: sets register to nil
    // A: target register
    LOP_LOADNIL,

    // LOADB: sets register to boolean and jumps to a given short offset (used to compile comparison results into a boolean)
    // A: target register
    // B: value (0/1)
    // C: jump offset
    LOP_LOADB,

    // LOADN: sets register to a number literal
    // A: target register
    // D: value (-32768..32767)
    LOP_LOADN,

    // LOADK: sets register to an entry from the constant table from the proto (number/vector/string)
    // A: target register
    // D: constant table index (0..32767)
    LOP_LOADK,

    // MOVE: move (copy) value from one register to another
    // A: target register
    // B: source register
    LOP_MOVE,

    // GETGLOBAL: load value from global table using constant string as a key
    // A: target register
    // C: predicted slot index (based on hash)
    // AUX: constant table index
    LOP_GETGLOBAL,

    // SETGLOBAL: set value in global table using constant string as a key
    // A: source register
    // C: predicted slot index (based on hash)
    // AUX: constant table index
    LOP_SETGLOBAL,

    // GETUPVAL: load upvalue from the upvalue table for the current function
    // A: target register
    // B: upvalue index
    LOP_GETUPVAL,

    // SETUPVAL: store value into the upvalue table for the current function
    // A: target register
    // B: upvalue index
    LOP_SETUPVAL,

    // CLOSEUPVALS: close (migrate to heap) all upvalues that were captured for registers >= target
    // A: target register
    LOP_CLOSEUPVALS,

    // GETIMPORT: load imported global table global from the constant table
    // A: target register
    // D: constant table index (0..32767); we assume that imports are loaded into the constant table
    // AUX: 3 10-bit indices of constant strings that, combined, constitute an import path; length of the path is set by the top 2 bits (1,2,3)
    LOP_GETIMPORT,

    // GETTABLE: load value from table into target register using key from register
    // A: target register
    // B: table register
    // C: index register
    LOP_GETTABLE,

    // SETTABLE: store source register into table using key from register
    // A: source register
    // B: table register
    // C: index register
    LOP_SETTABLE,

    // GETTABLEKS: load value from table into target register using constant string as a key
    // A: target register
    // B: table register
    // C: predicted slot index (based on hash)
    // AUX: constant table index
    LOP_GETTABLEKS,

    // SETTABLEKS: store source register into table using constant string as a key
    // A: source register
    // B: table register
    // C: predicted slot index (based on hash)
    // AUX: constant table index
    LOP_SETTABLEKS,

    // GETTABLEN: load value from table into target register using small integer index as a key
    // A: target register
    // B: table register
    // C: index-1 (index is 1..256)
    LOP_GETTABLEN,

    // SETTABLEN: store source register into table using small integer index as a key
    // A: source register
    // B: table register
    // C: index-1 (index is 1..256)
    LOP_SETTABLEN,

    // NEWCLOSURE: create closure from a child proto; followed by a CAPTURE instruction for each upvalue
    // A: target register
    // D: child proto index (0..32767)
    LOP_NEWCLOSURE,

    // NAMECALL: prepare to call specified method by name by loading function from source register using constant index into target register and copying source register into target register + 1
    // A: target register
    // B: source register
    // C: predicted slot index (based on hash)
    // AUX: constant table index
    // Note that this instruction must be followed directly by CALL; it prepares the arguments
    // This instruction is roughly equivalent to GETTABLEKS + MOVE pair, but we need a special instruction to support custom __namecall metamethod
    LOP_NAMECALL,

    // CALL: call specified function
    // A: register where the function object lives, followed by arguments; results are placed starting from the same register
    // B: argument count + 1, or 0 to preserve all arguments up to top (MULTRET)
    // C: result count + 1, or 0 to preserve all values and adjust top (MULTRET)
    LOP_CALL,

    // RETURN: returns specified values from the function
    // A: register where the returned values start
    // B: number of returned values + 1, or 0 to return all values up to top (MULTRET)
    LOP_RETURN,

    // JUMP: jumps to target offset
    // D: jump offset (-32768..32767; 0 means "next instruction" aka "don't jump")
    LOP_JUMP,

    // JUMPBACK: jumps to target offset; this is equivalent to JUMP but is used as a safepoint to be able to interrupt while/repeat loops
    // D: jump offset (-32768..32767; 0 means "next instruction" aka "don't jump")
    LOP_JUMPBACK,

    // JUMPIF: jumps to target offset if register is not nil/false
    // A: source register
    // D: jump offset (-32768..32767; 0 means "next instruction" aka "don't jump")
    LOP_JUMPIF,

    // JUMPIFNOT: jumps to target offset if register is nil/false
    // A: source register
    // D: jump offset (-32768..32767; 0 means "next instruction" aka "don't jump")
    LOP_JUMPIFNOT,

    // JUMPIFEQ, JUMPIFLE, JUMPIFLT, JUMPIFNOTEQ, JUMPIFNOTLE, JUMPIFNOTLT: jumps to target offset if the comparison is true (or false, for NOT variants)
    // A: source register 1
    // D: jump offset (-32768..32767; 1 means "next instruction" aka "don't jump")
    // AUX: source register 2
    LOP_JUMPIFEQ,
    LOP_JUMPIFLE,
    LOP_JUMPIFLT,
    LOP_JUMPIFNOTEQ,
    LOP_JUMPIFNOTLE,
    LOP_JUMPIFNOTLT,

    // ADD, SUB, MUL, DIV, MOD, POW: compute arithmetic operation between two source registers and put the result into target register
    // A: target register
    // B: source register 1
    // C: source register 2
    LOP_ADD,
    LOP_SUB,
    LOP_MUL,
    LOP_DIV,
    LOP_MOD,
    LOP_POW,

    // ADDK, SUBK, MULK, DIVK, MODK, POWK: compute arithmetic operation between the source register and a constant and put the result into target register
    // A: target register
    // B: source register
    // C: constant table index (0..255); must refer to a number
    LOP_ADDK,
    LOP_SUBK,
    LOP_MULK,
    LOP_DIVK,
    LOP_MODK,
    LOP_POWK,

    // AND, OR: perform `and` or `or` operation (selecting first or second register based on whether the first one is truthy) and put the result into target register
    // A: target register
    // B: source register 1
    // C: source register 2
    LOP_AND,
    LOP_OR,

    // ANDK, ORK: perform `and` or `or` operation (selecting source register or constant based on whether the source register is truthy) and put the result into target register
    // A: target register
    // B: source register
    // C: constant table index (0..255)
    LOP_ANDK,
    LOP_ORK,

    // CONCAT: concatenate all strings between B and C (inclusive) and put the result into A
    // A: target register
    // B: source register start
    // C: source register end
    LOP_CONCAT,

    // NOT, MINUS, LENGTH: compute unary operation for source register and put the result into target register
    // A: target register
    // B: source register
    LOP_NOT,
    LOP_MINUS,
    LOP_LENGTH,

    // NEWTABLE: create table in target register
    // A: target register
    // B: table size, stored as 0 for v=0 and ceil(log2(v))+1 for v!=0
    // AUX: array size
    LOP_NEWTABLE,

    // DUPTABLE: duplicate table using the constant table template to target register
    // A: target register
    // D: constant table index (0..32767)
    LOP_DUPTABLE,

    // SETLIST: set a list of values to table in target register
    // A: target register
    // B: source register start
    // C: value count + 1, or 0 to use all values up to top (MULTRET)
    // AUX: table index to start from
    LOP_SETLIST,

    // FORNPREP: prepare a numeric for loop, jump over the loop if first iteration doesn't need to run
    // A: target register; numeric for loops assume a register layout [limit, step, index, variable]
    // D: jump offset (-32768..32767)
    // limit/step are immutable, index isn't visible to user code since it's copied into variable
    LOP_FORNPREP,

    // FORNLOOP: adjust loop variables for one iteration, jump back to the loop header if loop needs to continue
    // A: target register; see FORNPREP for register layout
    // D: jump offset (-32768..32767)
    LOP_FORNLOOP,

    // FORGLOOP: adjust loop variables for one iteration of a generic for loop, jump back to the loop header if loop needs to continue
    // A: target register; generic for loops assume a register layout [generator, state, index, variables...]
    // D: jump offset (-32768..32767)
    // AUX: variable count (1..255) in the low 8 bits, high bit indicates whether to use ipairs-style traversal in the fast path
    // loop variables are adjusted by calling generator(state, index) and expecting it to return a tuple that's copied to the user variables
    // the first variable is then copied into index; generator/state are immutable, index isn't visible to user code
    LOP_FORGLOOP,

    // FORGPREP_INEXT: prepare FORGLOOP with 2 output variables (no AUX encoding), assuming generator is luaB_inext, and jump to FORGLOOP
    // A: target register (see FORGLOOP for register layout)
    // D: jump offset (-32768..32767)
    LOP_FORGPREP_INEXT,

    // FASTCALL3: perform a fast call of a built-in function using 3 register arguments
    // A: builtin function id (see LuauBuiltinFunction)
    // B: source argument register
    // C: jump offset to get to following CALL
    // AUX: source register 2 in least-significant byte
    // AUX: source register 3 in second least-significant byte
    LOP_FASTCALL3,

    // FORGPREP_NEXT: prepare FORGLOOP with 2 output variables (no AUX encoding), assuming generator is luaB_next, and jump to FORGLOOP
    // A: target register (see FORGLOOP for register layout)
    // D: jump offset (-32768..32767)
    LOP_FORGPREP_NEXT,

    // NATIVECALL: start executing new function in native code
    // this is a pseudo-instruction that is never emitted by bytecode compiler, but can be constructed at runtime to accelerate native code dispatch
    LOP_NATIVECALL,

    // GETVARARGS: copy variables into the target register from vararg storage for current function
    // A: target register
    // B: variable count + 1, or 0 to copy all variables and adjust top (MULTRET)
    LOP_GETVARARGS,

    // DUPCLOSURE: create closure from a pre-created function object (reusing it unless environments diverge)
    // A: target register
    // D: constant table index (0..32767)
    LOP_DUPCLOSURE,

    // PREPVARARGS: prepare stack for variadic functions so that GETVARARGS works correctly
    // A: number of fixed arguments
    LOP_PREPVARARGS,

    // LOADKX: sets register to an entry from the constant table from the proto (number/string)
    // A: target register
    // AUX: constant table index
    LOP_LOADKX,

    // JUMPX: jumps to the target offset; like JUMPBACK, supports interruption
    // E: jump offset (-2^23..2^23; 0 means "next instruction" aka "don't jump")
    LOP_JUMPX,

    // FASTCALL: perform a fast call of a built-in function
    // A: builtin function id (see LuauBuiltinFunction)
    // C: jump offset to get to following CALL
    // FASTCALL is followed by one of (GETIMPORT, MOVE, GETUPVAL) instructions and by CALL instruction
    // This is necessary so that if FASTCALL can't perform the call inline, it can continue normal execution
    // If FASTCALL *can* perform the call, it jumps over the instructions *and* over the next CALL
    // Note that FASTCALL will read the actual call arguments, such as argument/result registers and counts, from the CALL instruction
    LOP_FASTCALL,

    // COVERAGE: update coverage information stored in the instruction
    // E: hit count for the instruction (0..2^23-1)
    // The hit count is incremented by VM every time the instruction is executed, and saturates at 2^23-1
    LOP_COVERAGE,

    // CAPTURE: capture a local or an upvalue as an upvalue into a newly created closure; only valid after NEWCLOSURE
    // A: capture type, see LuauCaptureType
    // B: source register (for VAL/REF) or upvalue index (for UPVAL)
    LOP_CAPTURE,

    // SUBRK, DIVRK: compute arithmetic operation between the constant and a source register and put the result into target register
    // A: target register
    // B: constant table index (0..255); must refer to a number
    // C: source register
    LOP_SUBRK,
    LOP_DIVRK,

    // FASTCALL1: perform a fast call of a built-in function using 1 register argument
    // A: builtin function id (see LuauBuiltinFunction)
    // B: source argument register
    // C: jump offset to get to following CALL
    LOP_FASTCALL1,

    // FASTCALL2: perform a fast call of a built-in function using 2 register arguments
    // A: builtin function id (see LuauBuiltinFunction)
    // B: source argument register
    // C: jump offset to get to following CALL
    // AUX: source register 2 in least-significant byte
    LOP_FASTCALL2,

    // FASTCALL2K: perform a fast call of a built-in function using 1 register argument and 1 constant argument
    // A: builtin function id (see LuauBuiltinFunction)
    // B: source argument register
    // C: jump offset to get to following CALL
    // AUX: constant index
    LOP_FASTCALL2K,

    // FORGPREP: prepare loop variables for a generic for loop, jump to the loop backedge unconditionally
    // A: target register; generic for loops assume a register layout [generator, state, index, variables...]
    // D: jump offset (-32768..32767)
    LOP_FORGPREP,

    // JUMPXEQKNIL, JUMPXEQKB: jumps to target offset if the comparison with constant is true (or false, see AUX)
    // A: source register 1
    // D: jump offset (-32768..32767; 1 means "next instruction" aka "don't jump")
    // AUX: constant value (for boolean) in low bit, NOT flag (that flips comparison result) in high bit
    LOP_JUMPXEQKNIL,
    LOP_JUMPXEQKB,

    // JUMPXEQKN, JUMPXEQKS: jumps to target offset if the comparison with constant is true (or false, see AUX)
    // A: source register 1
    // D: jump offset (-32768..32767; 1 means "next instruction" aka "don't jump")
    // AUX: constant table index in low 24 bits, NOT flag (that flips comparison result) in high bit
    LOP_JUMPXEQKN,
    LOP_JUMPXEQKS,

    // IDIV: compute floor division between two source registers and put the result into target register
    // A: target register
    // B: source register 1
    // C: source register 2
    LOP_IDIV,

    // IDIVK compute floor division between the source register and a constant and put the result into target register
    // A: target register
    // B: source register
    // C: constant table index (0..255)
    LOP_IDIVK,

    // Atom-based userdata field access acceleration
    // These are equivalent to their GETTABLEKS/SETTABLEKS/NAMECALL counterparts, except tailored towards userdata field accesses
    // If the user has registered metamethods for a userdata tag, callbacks will be called by these instructions
    // NOTE: it uses only lower 2 bytes in AUX for constant index. Higher bytes are used for runtime cache.
    LOP_GETUDATAKS,
    LOP_SETUDATAKS,
    LOP_NAMECALLUDATA,

    // NEWCLASSMEMBER: register this method on a class object.
    // A: target register of class
    // B: reserved
    // C: initial value of this member. currently must be a function.
    // AUX: The name of this member as a constant string
    LOP_NEWCLASSMEMBER,

    // CALLFB: call specified function with collecting runtime stats in a feedback slot
    // A: register where the function object lives, followed by arguments; results are placed starting from the same register
    // B: argument count + 1, or 0 to preserve all arguments up to top (MULTRET)
    // C: result count + 1, or 0 to preserve all values and adjust top (MULTRET)
    // AUX: feedback slot id. 0xFFFFFFFF - sealed
    LOP_CALLFB,

    // CMPPROTO: check if a register contains a closure with a specified Luau function proto id
    // A: closure register
    // D: jump offset if proto doesn't match
    // AUX: proto id
    LOP_CMPPROTO,

    // FASTPCALL: perform a fastcall of a built-in protected call function
    // A: protected function id (0 - pcall, 1 - xpcall)
    // B: number of explicit arguments before a variadic tail
    // C: jump offset to get to following CALL
    LOP_FASTPCALL,


    // The following instructions appear only in experimental bytecode versions and are subject to change

    // NEWCLASS: reify a class object
    // A: target register of class
    // B: source register of superclass, or 0xFF if no superclass
    // C: bottom bit is 1 if the class is open, else 0; upper 7 bits are reserved
    // AUX: constant table index of unreified class object
    LOP_NEWCLASS,

    // CONSTRUCT: duplicate table to ra using the constant table template specified in the feedback slot. if rb is a class with default constructor and duplicated table matches shape required by the class, creates an object using class's default constructor
    // A: target register
    // B: register that is likely to hold a class
    // C: reserved
    // AUX: feedback slot id
    LOP_CONSTRUCT,

    // FINCONSTRUCT: checks if object construction initiated by a preceding CONSTRUCT succeeded and either finishes result placement or fallbacks to a CALL instruction
    // Note that FINCONSTRUCT will read the actual call arguments, such as argument/result registers and counts, from the CALL instruction
    // A: source register
    // B: reserved
    // C: jump offset to get to following CALL
    LOP_FINCONSTRUCT,

    // Enum entry for number of opcodes, not a valid opcode by itself!
    LOP__COUNT
};

// Bytecode instruction header: it's always a 32-bit integer, with low byte (first byte in little endian) containing the opcode
// Some instruction types require more data and have more 32-bit integers following the header
#define LUAU_INSN_OP(insn) ((insn) & 0xff)

// ABC encoding: three 8-bit values, containing registers or small numbers
#define LUAU_INSN_A(insn) (((insn) >> 8) & 0xff)
#define LUAU_INSN_B(insn) (((insn) >> 16) & 0xff)
#define LUAU_INSN_C(insn) (((insn) >> 24) & 0xff)

// AD encoding: one 8-bit value, one signed 16-bit value
#define LUAU_INSN_D(insn) (int32_t(insn) >> 16)

// E encoding: one signed 24-bit value
#define LUAU_INSN_E(insn) (int32_t(insn) >> 8)

// Auxiliary AB: two 8-bit values, containing registers or small numbers
// Used in FASTCALL3
#define LUAU_INSN_AUX_A(aux) ((aux) & 0xff)
#define LUAU_INSN_AUX_B(aux) (((aux) >> 8) & 0xff)

// Auxiliary KV: unsigned 24-bit constant index
// Used in LOP_JUMPXEQK* instructions
#define LUAU_INSN_AUX_KV(aux) ((aux) & 0xffffff)

// Auxiliary KB: 1-bit constant value
// Used in LOP_JUMPXEQKB instruction
#define LUAU_INSN_AUX_KB(aux) ((aux) & 0x1)

// Auxiliary NOT: 1-bit negation flag
// Used in LOP_JUMPXEQK* instructions
#define LUAU_INSN_AUX_NOT(aux) ((aux) >> 31)

// Auxiliary 16-bit constant index and 16-bit cachedslot
// Used in LOP_GETUDATAKS, LOP_SETUDATAKS and LOP_NAMECALLUDATA
#define LUAU_INSN_AUX_KV16(aux) ((aux) & 0xffffu)
#define LUAU_INSN_AUX_SLOT(aux) ((aux) >> 16)

#define LUAU_INSN_FBSLOT_SEALED 0xFFFFFFFF

// Bytecode tags, used internally for bytecode encoded as a string
enum LuauBytecodeTag
{
    // Bytecode version; runtime supports [MIN, MAX], compiler emits TARGET by default but may emit a higher version when flags are enabled
    LBC_VERSION_MIN = 3,
    LBC_VERSION_MAX = 14,
    LBC_VERSION_TARGET = 9,
    LBC_VERSION_CLASSES = 100,
    // Type encoding version
    LBC_TYPE_VERSION_MIN = 1,
    LBC_TYPE_VERSION_MAX = 3,
    LBC_TYPE_VERSION_TARGET = 3,
    // Types of constant table entries
    LBC_CONSTANT_NIL = 0,
    LBC_CONSTANT_BOOLEAN,
    LBC_CONSTANT_NUMBER,
    LBC_CONSTANT_STRING,
    LBC_CONSTANT_IMPORT,
    LBC_CONSTANT_TABLE,
    LBC_CONSTANT_CLOSURE,
    LBC_CONSTANT_VECTOR,
    LBC_CONSTANT_TABLE_WITH_CONSTANTS,
    LBC_CONSTANT_INTEGER,
    LBC_CONSTANT_CLASS_SHAPE,
    LBC_CONSTANT_VECTORD,

    /** WARNING: This must always be last. */
    LBC_CONSTANT__COUNT
};

// Type table tags
enum LuauBytecodeType
{
    LBC_TYPE_NIL = 0,
    LBC_TYPE_BOOLEAN,
    LBC_TYPE_NUMBER,
    LBC_TYPE_STRING,
    LBC_TYPE_TABLE,
    LBC_TYPE_FUNCTION,
    LBC_TYPE_THREAD,
    LBC_TYPE_USERDATA,
    LBC_TYPE_VECTOR,
    LBC_TYPE_BUFFER,
    LBC_TYPE_INTEGER,
    LBC_TYPE_CLASS,
    LBC_TYPE_OBJECT,

    LBC_TYPE_ANY = 15,

    LBC_TYPE_TAGGED_USERDATA_BASE = 64,
    LBC_TYPE_TAGGED_USERDATA_END = 64 + 32,

    LBC_TYPE_OPTIONAL_BIT = 1 << 7,

    LBC_TYPE_INVALID = 256,
};

// Builtin function ids, used in LOP_FASTCALL
enum LuauBuiltinFunction
{
    LBF_NONE = 0,

    // assert()
    LBF_ASSERT,

    // math.
    LBF_MATH_ABS,
    LBF_MATH_ACOS,
    LBF_MATH_ASIN,
    LBF_MATH_ATAN2,
    LBF_MATH_ATAN,
    LBF_MATH_CEIL,
    LBF_MATH_COSH,
    LBF_MATH_COS,
    LBF_MATH_DEG,
    LBF_MATH_EXP,
    LBF_MATH_FLOOR,
    LBF_MATH_FMOD,
    LBF_MATH_FREXP,
    LBF_MATH_LDEXP,
    LBF_MATH_LOG10,
    LBF_MATH_LOG,
    LBF_MATH_MAX,
    LBF_MATH_MIN,
    LBF_MATH_MODF,
    LBF_MATH_POW,
    LBF_MATH_RAD,
    LBF_MATH_SINH,
    LBF_MATH_SIN,
    LBF_MATH_SQRT,
    LBF_MATH_TANH,
    LBF_MATH_TAN,

    // bit32.
    LBF_BIT32_ARSHIFT,
    LBF_BIT32_BAND,
    LBF_BIT32_BNOT,
    LBF_BIT32_BOR,
    LBF_BIT32_BXOR,
    LBF_BIT32_BTEST,
    LBF_BIT32_EXTRACT,
    LBF_BIT32_LROTATE,
    LBF_BIT32_LSHIFT,
    LBF_BIT32_REPLACE,
    LBF_BIT32_RROTATE,
    LBF_BIT32_RSHIFT,

    // type()
    LBF_TYPE,

    // string.
    LBF_STRING_BYTE,
    LBF_STRING_CHAR,
    LBF_STRING_LEN,

    // typeof()
    LBF_TYPEOF,

    // string.
    LBF_STRING_SUB,

    // math.
    LBF_MATH_CLAMP,
    LBF_MATH_SIGN,
    LBF_MATH_ROUND,

    // raw*
    LBF_RAWSET,
    LBF_RAWGET,
    LBF_RAWEQUAL,

    // table.
    LBF_TABLE_INSERT,
    LBF_TABLE_UNPACK,

    // vector ctor
    LBF_VECTOR,

    // bit32.count
    LBF_BIT32_COUNTLZ,
    LBF_BIT32_COUNTRZ,

    // select(_, ...)
    LBF_SELECT_VARARG,

    // rawlen
    LBF_RAWLEN,

    // bit32.extract(_, k, k)
    LBF_BIT32_EXTRACTK,

    // get/setmetatable
    LBF_GETMETATABLE,
    LBF_SETMETATABLE,

    // tonumber/tostring
    LBF_TONUMBER,
    LBF_TOSTRING,

    // bit32.byteswap(n)
    LBF_BIT32_BYTESWAP,

    // buffer.
    LBF_BUFFER_READI8,
    LBF_BUFFER_READU8,
    LBF_BUFFER_WRITEU8,
    LBF_BUFFER_READI16,
    LBF_BUFFER_READU16,
    LBF_BUFFER_WRITEU16,
    LBF_BUFFER_READI32,
    LBF_BUFFER_READU32,
    LBF_BUFFER_WRITEU32,
    LBF_BUFFER_READF32,
    LBF_BUFFER_WRITEF32,
    LBF_BUFFER_READF64,
    LBF_BUFFER_WRITEF64,

    // vector.
    LBF_VECTOR_MAGNITUDE,
    LBF_VECTOR_NORMALIZE,
    LBF_VECTOR_CROSS,
    LBF_VECTOR_DOT,
    LBF_VECTOR_FLOOR,
    LBF_VECTOR_CEIL,
    LBF_VECTOR_ABS,
    LBF_VECTOR_SIGN,
    LBF_VECTOR_CLAMP,
    LBF_VECTOR_MIN,
    LBF_VECTOR_MAX,

    // math.lerp
    LBF_MATH_LERP,

    // vector.lerp
    LBF_VECTOR_LERP,

    // math.
    LBF_MATH_ISNAN,
    LBF_MATH_ISINF,
    LBF_MATH_ISFINITE,

    // integer
    LBF_INTEGER_CREATE,
    LBF_INTEGER_TONUMBER,
    LBF_INTEGER_NEG,
    LBF_INTEGER_ADD,
    LBF_INTEGER_SUB,
    LBF_INTEGER_MUL,
    LBF_INTEGER_DIV,
    LBF_INTEGER_MIN,
    LBF_INTEGER_MAX,
    LBF_INTEGER_REM,
    LBF_INTEGER_IDIV,
    LBF_INTEGER_UDIV,
    LBF_INTEGER_UREM,
    LBF_INTEGER_MOD,
    LBF_INTEGER_CLAMP,
    LBF_INTEGER_BAND,
    LBF_INTEGER_BOR,
    LBF_INTEGER_BNOT,
    LBF_INTEGER_BXOR,
    LBF_INTEGER_LT,
    LBF_INTEGER_LE,
    LBF_INTEGER_ULT,
    LBF_INTEGER_ULE,
    LBF_INTEGER_GT,
    LBF_INTEGER_GE,
    LBF_INTEGER_UGT,
    LBF_INTEGER_UGE,
    LBF_INTEGER_LSHIFT,
    LBF_INTEGER_RSHIFT,
    LBF_INTEGER_ARSHIFT,
    LBF_INTEGER_LROTATE,
    LBF_INTEGER_RROTATE,
    LBF_INTEGER_EXTRACT,
    LBF_INTEGER_BTEST,
    LBF_INTEGER_COUNTRZ,
    LBF_INTEGER_COUNTLZ,
    LBF_INTEGER_BSWAP,

    // buffer.readinteger / buffer.writeinteger (int64_t)
    LBF_BUFFER_READINTEGER,
    LBF_BUFFER_WRITEINTEGER,
};

// Capture type, used in LOP_CAPTURE
enum LuauCaptureType
{
    LCT_VAL = 0,
    LCT_REF,
    LCT_UPVAL,
};

// Proto flag bitmask, stored in Proto::flags
enum LuauProtoFlag
{
    // used to tag main proto for modules with --!native
    LPF_NATIVE_MODULE = 1 << 0,
    // used to tag individual protos as not profitable to compile natively
    LPF_NATIVE_COLD = 1 << 1,
    // used to tag main proto for modules that have at least one function with native attribute
    LPF_NATIVE_FUNCTION = 1 << 2,
    // function can be inlined
    LPF_INLINABLE = 1 << 3,
    // top-level function uses export statements and returns the export table
    LPF_USES_EXPORT = 1 << 4,
};

enum LuauFeedbackType
{
    LFT_CALLTARGET = 0,

    // The following enumerations appear only in experimental bytecode versions and are subject to change
    LFT_CONSTRUCT = 1,
};
