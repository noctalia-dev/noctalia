#include "Luau/BytecodeBuilder.h"
#include "Luau/BytecodeGraph.h"
#include "Luau/BytecodeWire.h"

#include "BytecodeGraphParser.h"
#include "BytecodeGraphSerializer.h"

LUAU_FASTFLAG(DebugLuauUserDefinedClasses)
LUAU_FASTFLAG(LuauCostModel)
LUAU_FASTFLAG(LuauCompileRefactorFeedback)

namespace Luau
{
namespace Bytecode
{

static std::string_view readString(std::vector<std::string_view>& strings, const char* data, size_t& offset)
{
    uint32_t stringId = readVarInt(data, offset);
    LUAU_ASSERT(stringId <= strings.size());
    if (stringId == 0)
        return "";
    return strings[stringId - 1];
}

std::optional<CompTimeBcFunction> fromFunctionBytecode(std::string bytecode, std::vector<std::string_view>& strings)
{
    CompTimeBcFunction fn;
    size_t offset = 0;
    const char* data = bytecode.data();
    fn.maxstacksize = read<uint8_t>(data, offset);
    fn.numparams = read<uint8_t>(data, offset);
    fn.nups = read<uint8_t>(data, offset);
    fn.is_vararg = read<uint8_t>(data, offset);
    fn.flags = read<uint8_t>(data, offset);

    uint32_t typesSize = readVarInt(data, offset);
    if (typesSize > 0)
    {
        uint32_t typeInfoSize = readVarInt(data, offset);
        uint32_t typedUpvalSize = readVarInt(data, offset);
        uint32_t typedLocalSize = readVarInt(data, offset);
        fn.typeInfo = bytecode.substr(offset, typeInfoSize);
        offset += typeInfoSize;

        fn.upvalueTypes.resize(typedUpvalSize);
        for (uint32_t i = 0; i < typedUpvalSize; i++)
            fn.upvalueTypes[i] = static_cast<LuauBytecodeType>(read<uint8_t>(data, offset));

        fn.localTypes.resize(typedLocalSize);
        for (uint32_t i = 0; i < typedLocalSize; i++)
        {
            LuauBytecodeType type = static_cast<LuauBytecodeType>(read<uint8_t>(data, offset));
            uint8_t reg = read<uint8_t>(data, offset);
            uint32_t startpc = readVarInt(data, offset);
            uint32_t endpc = startpc + readVarInt(data, offset);
            fn.localTypes[i] = {type, reg, startpc, endpc};
        }
    }

    // store pointer to bytecode
    int32_t codesize = readVarInt(data, offset);
    const Instruction* code = reinterpret_cast<const Instruction*>(data + offset);

    offset += codesize * sizeof(Instruction);

    // read constants
    const uint32_t sizek = readVarInt(data, offset);
    fn.constants.resize(sizek);
    for (uint32_t i = 0; i < sizek; i++)
    {
        uint8_t constType = read<uint8_t>(data, offset);
        switch (constType)
        {
        case LBC_CONSTANT_NIL:
            fn.constants[i].kind = BcVmConstKind::Nil;
            break;

        case LBC_CONSTANT_BOOLEAN:
        {
            fn.constants[i].kind = BcVmConstKind::Boolean;
            fn.constants[i].valueBoolean = read<uint8_t>(data, offset);
            break;
        }

        case LBC_CONSTANT_NUMBER:
        {
            fn.constants[i].kind = BcVmConstKind::Number;
            fn.constants[i].valueNumber = read<double>(data, offset);
            break;
        }

        case LBC_CONSTANT_VECTOR:
        {
            fn.constants[i].kind = BcVmConstKind::Vectorf;
            fn.constants[i].valueVectorf[0] = read<float>(data, offset);
            fn.constants[i].valueVectorf[1] = read<float>(data, offset);
            fn.constants[i].valueVectorf[2] = read<float>(data, offset);
            fn.constants[i].valueVectorf[3] = read<float>(data, offset);
            break;
        }

        case LBC_CONSTANT_VECTORD:
        {
            fn.constants[i].kind = BcVmConstKind::Vectord;
            fn.constants[i].valueVectord[0] = read<double>(data, offset);
            fn.constants[i].valueVectord[1] = read<double>(data, offset);
            fn.constants[i].valueVectord[2] = read<double>(data, offset);
            fn.constants[i].valueVectord[3] = read<double>(data, offset);
            break;
        }

        case LBC_CONSTANT_STRING:
        {
            fn.constants[i].kind = BcVmConstKind::String;
            fn.constants[i].valueString = readString(strings, data, offset);
            break;
        }

        case LBC_CONSTANT_IMPORT:
        {
            fn.constants[i].kind = BcVmConstKind::Import;
            fn.constants[i].valueImport = read<uint32_t>(data, offset);
            break;
        }

        case LBC_CONSTANT_TABLE:
        case LBC_CONSTANT_TABLE_WITH_CONSTANTS:
        {
            fn.constants[i].kind = BcVmConstKind::Table;
            fn.constants[i].valueTable = uint32_t(fn.tableShapes.size());

            BytecodeBuilder::TableShape shape;
            shape.length = readVarInt(data, offset);
            shape.hasConstants = constType == LBC_CONSTANT_TABLE_WITH_CONSTANTS;

            for (uint32_t i = 0; i < shape.length; ++i)
            {
                uint32_t key = readVarInt(data, offset);
                LUAU_ASSERT(key < sizek);
                shape.keys[i] = key;
                if (shape.hasConstants)
                {
                    int32_t value = read<int32_t>(data, offset);
                    LUAU_ASSERT(value < static_cast<int32_t>(sizek));
                    shape.constants[i] = value;
                }
            }
            fn.tableShapes.push_back(shape);
            break;
        }

        case LBC_CONSTANT_CLOSURE:
        {
            fn.constants[i].kind = BcVmConstKind::Closure;
            fn.constants[i].valueClosure = readVarInt(data, offset);
            break;
        }

        case LBC_CONSTANT_INTEGER:
        {
            fn.constants[i].kind = BcVmConstKind::Integer;
            bool isNegative = read<uint8_t>(data, offset);
            uint64_t magnitude = readVarInt64(data, offset);
            fn.constants[i].valueInteger = isNegative ? (int64_t)(~magnitude + 1) : (int64_t)magnitude;
            break;
        }

        case LBC_CONSTANT_CLASS_SHAPE:
        {
            LUAU_ASSERT(FFlag::DebugLuauUserDefinedClasses);

            fn.constants[i].kind = BcVmConstKind::ClassShape;
            fn.constants[i].valueClassShape = uint32_t(fn.classShapes.size());

            BytecodeBuilder::ClassShape shape;
            shape.className = readVarInt(data, offset);

            uint32_t numProps = readVarInt(data, offset);
            uint32_t numMethods = readVarInt(data, offset);

            shape.propertyNames.reserve(numProps);
            shape.methodNames.reserve(numMethods);

            for (uint32_t i = 0; i < numProps; ++i)
                shape.propertyNames.emplace_back(readVarInt(data, offset));

            for (uint32_t i = 0; i < numMethods; ++i)
                shape.methodNames.emplace_back(readVarInt(data, offset));

            fn.classShapes.push_back(shape);
            break;
        }

        default:
            LUAU_ASSERT(!"Unknown constant type!");
        }
    }

    uint32_t psize = readVarInt(data, offset);
    fn.protos.resize(psize);
    for (uint32_t i = 0; i < psize; i++)
        fn.protos[i] = readVarInt(data, offset);

    fn.linedefined = readVarInt(data, offset);
    fn.debugname = readString(strings, data, offset);

    uint8_t lineinfo = read<uint8_t>(data, offset);
    std::vector<uint32_t> lines;

    if (lineinfo != 0)
    {
        uint8_t linegaplog2 = read<uint8_t>(data, offset);

        int intervals = ((codesize - 1) >> linegaplog2) + 1;
        int absoffset = (codesize + 3) & ~3;

        const int sizelineinfo = absoffset + intervals * sizeof(int);
        std::vector<uint8_t> lineinfo;
        lineinfo.resize(sizelineinfo);
        int* abslineinfo = reinterpret_cast<int*>(lineinfo.data() + absoffset);

        uint8_t lastoffset = 0;
        for (int i = 0; i < codesize; i++)
        {
            lastoffset += read<uint8_t>(data, offset);
            lineinfo[i] = lastoffset;
        }

        int lastline = 0;
        for (int i = 0; i < intervals; i++)
        {
            lastline += read<int32_t>(data, offset);
            abslineinfo[i] = lastline;
        }
        lines.resize(codesize);
        for (int i = 0; i < codesize; i++)
            lines[i] = abslineinfo[i >> linegaplog2] + lineinfo[i];
    }

    uint8_t debuginfo = read<uint8_t>(data, offset);

    if (debuginfo != 0)
    {
        const int sizelocvars = readVarInt(data, offset);
        fn.locals.resize(sizelocvars);

        for (int i = 0; i < sizelocvars; i++)
        {
            std::string_view varname = readString(strings, data, offset);
            uint32_t startpc = readVarInt(data, offset);
            uint32_t endpc = readVarInt(data, offset);
            uint8_t reg = read<uint8_t>(data, offset);
            fn.locals[i] = {varname, reg, startpc, endpc};
        }

        const int sizeupvalues = readVarInt(data, offset);
        fn.upvalueNames.resize(sizeupvalues);

        for (int i = 0; i < sizeupvalues; i++)
            fn.upvalueNames[i] = readString(strings, data, offset);
    }

    uint32_t feedbackvecsize = readVarInt(data, offset);
    fn.feedbackSlots.resize(feedbackvecsize);

    for (uint32_t j = 0; j < feedbackvecsize; j++)
    {
        uint8_t slottype = read<uint8_t>(data, offset);

        BcFeedbackSlot& slot = fn.feedbackSlots[j];
        slot.kind = static_cast<LuauFeedbackType>(slottype);

        if (slottype == LFT_CALLTARGET)
        {
            uint32_t pc = readVarInt(data, offset);
            LUAU_ASSERT(pc < uint32_t(codesize));
            LUAU_ASSERT(LUAU_INSN_OP(code[pc]) == LOP_CALLFB);
            slot.callTarget.inst = pc;
        }
        else if (slottype == LFT_CONSTRUCT)
        {
            uint32_t shape = readVarInt(data, offset);
            LUAU_ASSERT(shape < fn.constants.size());
            LUAU_ASSERT(fn.constants[shape].kind == BcVmConstKind::Table);
            slot.construct.shape = shape;
        }
        else
        {
            LUAU_ASSERT(!"unknown feedback slot kind");
        }
    }

    if (FFlag::LuauCostModel)
    {
        if ((fn.flags & LPF_INLINABLE) != 0)
            readVarInt64(data, offset);
    }

    std::vector<uint32_t> insnsPC;
    BytecodeGraphParser<BcVmConst> graphParser(fn);
    if (!graphParser.rebuildGraph(code, codesize, lines, insnsPC))
        return {};

    for (BcFeedbackSlot& slot : fn.feedbackSlots)
    {
        if (slot.kind == LFT_CALLTARGET)
            slot.callTarget.inst = insnsPC[slot.callTarget.inst];
    }

    for (TypedLocal& l : fn.localTypes)
    {
        l.startpc = l.startpc < insnsPC.size() ? insnsPC[l.startpc] : codesize;
        l.endpc = l.endpc < insnsPC.size() ? insnsPC[l.endpc] : codesize;
    }

    for (DebugLocal& l : fn.locals)
    {
        l.startpc = l.startpc < insnsPC.size() ? insnsPC[l.startpc] : codesize;
        l.endpc = l.endpc < insnsPC.size() ? insnsPC[l.endpc] : codesize;
    }

    return {fn};
}

struct CompTimeBytecodeGraphSerializer : public BytecodeGraphSerializer<BcVmConst>
{
    std::vector<uint32_t>& consts;
    CompTimeBytecodeGraphSerializer(BytecodeBuilder& bcb, CompTimeBcFunction& fn, std::vector<uint32_t>& consts)
        : BytecodeGraphSerializer<BcVmConst>(bcb, fn)
        , consts(consts)
    {
    }

    uint32_t getVmConstInputRaw(BcInst& insn, uint8_t index) override
    {
        uint32_t cid = BytecodeGraphSerializer<BcVmConst>::getVmConstInputRaw(insn, index);
        LUAU_ASSERT(cid < consts.size());
        return consts[cid];
    }
};

std::string toFunctionBytecode(BytecodeBuilder& bcb, CompTimeBcFunction& fn)
{
    uint32_t functionId = bcb.beginFunction(fn.numparams, fn.is_vararg);
    if (fn.debugname != "")
        bcb.setDebugFunctionName({fn.debugname.data(), fn.debugname.size()});
    bcb.setDebugFunctionLineDefined(fn.linedefined);
    bcb.setFunctionTypeInfo(fn.typeInfo);
    for (LuauBytecodeType t : fn.upvalueTypes)
        bcb.pushUpvalTypeInfo(t);
    for (auto& upval : fn.upvalueNames)
        bcb.pushDebugUpval({upval.data(), upval.size()});

    std::vector<uint32_t> consts;
    consts.reserve(fn.constants.size());

    for (auto& c : fn.constants)
    {
        switch (c.kind)
        {
        case BcVmConstKind::Nil:
            consts.push_back(bcb.addConstantNil());
            break;

        case BcVmConstKind::Boolean:
            consts.push_back(bcb.addConstantBoolean(c.valueBoolean));
            break;

        case BcVmConstKind::Number:
            consts.push_back(bcb.addConstantNumber(c.valueNumber));
            break;

        case BcVmConstKind::Vectorf:
            consts.push_back(bcb.addConstantVectorf(c.valueVectorf[0], c.valueVectorf[1], c.valueVectorf[2], c.valueVectorf[3]));
            break;

        case BcVmConstKind::Vectord:
            consts.push_back(bcb.addConstantVectord(c.valueVectord[0], c.valueVectord[1], c.valueVectord[2], c.valueVectord[3]));
            break;

        case BcVmConstKind::String:
            consts.push_back(bcb.addConstantString({c.valueString.data(), c.valueString.size()}));
            break;

        case BcVmConstKind::Import:
            consts.push_back(bcb.addImport(c.valueImport));
            break;

        case BcVmConstKind::Table:
        {
            LUAU_ASSERT(c.valueTable < fn.tableShapes.size());
            consts.push_back(bcb.addConstantTable(fn.tableShapes[c.valueTable]));
            break;
        }

        case BcVmConstKind::Closure:
            consts.push_back(bcb.addConstantClosure(c.valueClosure));
            break;

        case BcVmConstKind::Integer:
            consts.push_back(bcb.addConstantInteger(c.valueInteger));
            break;

        case BcVmConstKind::ClassShape:
        {
            LUAU_ASSERT(FFlag::DebugLuauUserDefinedClasses);
            LUAU_ASSERT(c.valueClassShape < fn.classShapes.size());
            consts.push_back(bcb.addClassShape(fn.classShapes[c.valueClassShape]));
            break;
        }
        }
    }

    for (auto fid : fn.protos)
        bcb.addChildFunction(fid);

    CompTimeBytecodeGraphSerializer serializer(bcb, fn, consts);
    std::vector<uint32_t> insnsPC = serializer.emitBytecode();

    for (uint32_t i = 0; i < fn.feedbackSlots.size(); ++i)
    {
        BcFeedbackSlot& slot = fn.feedbackSlots[i];
        uint32_t slotId = ~0u;

        if (slot.kind == LFT_CALLTARGET)
        {
            LUAU_ASSERT(slot.callTarget.inst < insnsPC.size());
            LUAU_ASSERT(insnsPC[slot.callTarget.inst] != ~0u);

            if (FFlag::LuauCompileRefactorFeedback || FFlag::DebugLuauUserDefinedClasses)
                slotId = bcb.addCallTargetSlot(insnsPC[slot.callTarget.inst]);
            else
                slotId = bcb.addFbSlot_DEPRECATED(LFT_CALLTARGET, insnsPC[slot.callTarget.inst]);
        }
        else if (slot.kind == LFT_CONSTRUCT)
        {
            LUAU_ASSERT(slot.construct.shape < consts.size());
            slotId = bcb.addConstructSlot(consts[slot.construct.shape]);
        }
        else
        {
            LUAU_ASSERT(!"unknown feedback slot kind");
        }

        LUAU_ASSERT(slotId == i);
    }

    for (auto& local : fn.localTypes)
    {
        uint32_t startpc = local.startpc < insnsPC.size() ? insnsPC[local.startpc] : bcb.getDebugPC();
        uint32_t endpc = local.endpc < insnsPC.size() ? insnsPC[local.endpc] : bcb.getDebugPC();
        bcb.pushLocalTypeInfo(local.type, local.reg, startpc, endpc);
    }
    for (auto& local : fn.locals)
    {
        uint32_t startpc = local.startpc < insnsPC.size() ? insnsPC[local.startpc] : bcb.getDebugPC();
        uint32_t endpc = local.endpc < insnsPC.size() ? insnsPC[local.endpc] : bcb.getDebugPC();
        bcb.pushDebugLocal({local.varname.data(), local.varname.size()}, local.reg, startpc, endpc);
    }

    bcb.foldJumps();

    bool hasLongJumpError = false;
    bcb.expandJumps(hasLongJumpError);

    if (hasLongJumpError)
        return "";

    bcb.endFunction(fn.maxstacksize, fn.nups, fn.flags);

    if (serializer.error)
        return "";

    return bcb.getFunctionData(functionId);
}

std::string toFunctionBytecode(CompTimeBcFunction& fn)
{
    BytecodeBuilder bcb;
    return toFunctionBytecode(bcb, fn);
}

}; // namespace Bytecode
}; // namespace Luau
