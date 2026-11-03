// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/Bytecode.h"
#include "Luau/DenseHash.h"
#include "Luau/StringUtils.h"

#include <string>

namespace Luau
{

static const uint32_t kMaxRegisterCount = 255;
static const uint32_t kMaxUpvalueCount = 200;
static const uint32_t kMaxLocalCount = 200;
static const uint32_t kMaxInstructionCount = 1'000'000'000;

static const uint8_t kInvalidReg = 255;

static const uint32_t kMaxConstantCount = 1 << 23;
static const uint32_t kMaxClosureCount = 1 << 15;

static const int kMaxJumpDistance = 1 << 23;

class BytecodeEncoder
{
public:
    virtual ~BytecodeEncoder() {}

    virtual void encode(uint32_t* data, size_t count) = 0;
};

class BytecodeBuilder
{
public:
    // BytecodeBuilder does *not* copy the data passed via StringRef; instead, it keeps the ref around until finalize()
    // Please be careful with the lifetime of the data that's being passed because of this.
    // The safe and correct pattern is to only build StringRefs out of pieces of AST (AstName or AstArray<>) that are backed by AstAllocator.
    // Note that you must finalize() the builder before the Allocator backing the Ast is destroyed.
    struct StringRef
    {
        // To construct a StringRef, use sref() from Compiler.cpp.
        const char* data = nullptr;
        size_t length = 0;

        bool operator==(const StringRef& other) const;
    };

    struct TableShape
    {
        static const unsigned int kMaxLength = 32;

        int32_t keys[kMaxLength];
        // constants are indices that correspond to the proto constant table
        // if a key does not have an associated constant to fill in, it has a sentinel value of -1
        int32_t constants[kMaxLength];
        unsigned int length = 0;
        bool hasConstants = false;

        bool operator==(const TableShape& other) const;
    };

    struct ClassShape
    {
        int32_t className;
        std::vector<int32_t> propertyNames;
        std::vector<int32_t> methodNames;
    };

    BytecodeBuilder(BytecodeEncoder* encoder = 0);
    virtual ~BytecodeBuilder() = default;

    uint32_t beginFunction(uint8_t numparams, bool isvararg = false);
    void endFunction(uint8_t maxstacksize, uint8_t numupvalues, uint8_t flags = 0, uint64_t cost = 0);

    void setMainFunction(uint32_t fid);

    int32_t addConstantNil();
    int32_t addConstantBoolean(bool value);
    int32_t addConstantNumber(double value);
    int32_t addConstantInteger(int64_t value);
    int32_t addConstantVectorf(float x, float y, float z, float w);
    int32_t addConstantVectord(double x, double y, double z, double w);
    int32_t addConstantString(StringRef value);
    int32_t addImport(uint32_t iid);
    int32_t addConstantTable(const TableShape& shape);
    int32_t addConstantClosure(uint32_t fid);

    uint32_t addFbSlot_DEPRECATED(LuauFeedbackType t);
    uint32_t addFbSlot_DEPRECATED(LuauFeedbackType t, uint32_t pc);
    uint32_t addCallTargetSlot(uint32_t pc);
    uint32_t addConstructSlot(uint32_t shapecid);

    int16_t addChildFunction(uint32_t fid);
    int32_t addClassShape(ClassShape shape);

    void emitABC(LuauOpcode op, uint8_t a, uint8_t b, uint8_t c);
    void emitAD(LuauOpcode op, uint8_t a, int16_t d);
    void emitE(LuauOpcode op, int32_t e);
    void emitAux(uint32_t aux);

    void undoEmit(LuauOpcode op);
    unsigned lastInstruction();

    size_t emitLabel();

    [[nodiscard]] bool patchJumpD(size_t jumpLabel, size_t targetLabel);
    [[nodiscard]] bool patchSkipC(size_t jumpLabel, size_t targetLabel);

    void patchAux(size_t targetAux, int32_t newValue);

    void foldJumps();
    std::vector<uint32_t> expandJumps(bool& hasLongJumpError);

    void setFunctionTypeInfo(std::string value);
    void pushLocalTypeInfo(LuauBytecodeType type, uint8_t reg, uint32_t startpc, uint32_t endpc);
    void pushUpvalTypeInfo(LuauBytecodeType type);

    uint32_t addUserdataType(const char* name);
    void useUserdataType(uint32_t index);

    void setDebugFunctionName(StringRef name);
    void setDebugFunctionLineDefined(int line);
    void setDebugLine(int line);
    void pushDebugLocal(StringRef name, uint8_t reg, uint32_t startpc, uint32_t endpc);
    void pushDebugUpval(StringRef name);

    size_t getInstructionCount() const;
    size_t getTotalInstructionCount() const;
    uint32_t getDebugPC() const;

    void addDebugRemark(const char* format, ...) LUAU_PRINTF_ATTR(2, 3);

    void finalize();

    enum DumpFlags
    {
        Dump_Code = 1 << 0,
        Dump_Lines = 1 << 1,
        Dump_Source = 1 << 2,
        Dump_Locals = 1 << 3,
        Dump_Remarks = 1 << 4,
        Dump_Types = 1 << 5,
        Dump_Constants = 1 << 6,
    };

    void setDumpFlags(uint32_t flags)
    {
        dumpFlags = flags;
        dumpFunctionPtr = &BytecodeBuilder::dumpCurrentFunction;
    }

    void setDumpSource(const std::string& source);

    bool needsDebugRemarks() const
    {
        return (dumpFlags & Dump_Remarks) != 0;
    }

    const std::string& getBytecode() const
    {
        LUAU_ASSERT(!bytecode.empty()); // did you forget to call finalize?
        return bytecode;
    }

    std::string dumpFunction(uint32_t id) const;
    std::string dumpEverything() const;
    std::string dumpSourceRemarks() const;
    std::string dumpTypeInfo() const;

    std::string getFunctionData(uint32_t id)
    {
        return functions[id].data;
    }

    uint32_t getFunctionCount() const
    {
        return static_cast<uint32_t>(functions.size());
    }

    std::vector<std::string_view> getStringTable();

    void annotateInstruction(std::string& result, uint32_t fid, uint32_t instpos) const;

    void clearStrings()
    {
        debugStrings.clear();
        stringTable.clear();
    }

    static uint32_t getImportId(int32_t id0);
    static uint32_t getImportId(int32_t id0, int32_t id1);
    static uint32_t getImportId(int32_t id0, int32_t id1, int32_t id2);

    static int decomposeImportId(uint32_t ids, int32_t& id0, int32_t& id1, int32_t& id2);

    static uint32_t getStringHash(StringRef key);

    static std::string getError(const std::string& message);

    static uint8_t getVersion();
    static uint8_t getTypeEncodingVersion();

protected:
    struct Constant
    {
        enum Type
        {
            Type_Nil,
            Type_Boolean,
            Type_Number,
            Type_Integer,
            Type_Vectorf,
            Type_Vectord,
            Type_String,
            Type_Import,
            Type_Table,
            Type_Closure,
            Type_ClassShape,
        };

        Type type;
        union
        {
            bool valueBoolean;
            double valueNumber;
            int64_t valueInteger64;
            float valueVectorf[4];
            double valueVectord[4];
            unsigned int valueString; // index into string table
            uint32_t valueImport;     // 10-10-10-2 encoded import id
            uint32_t valueTable;      // index into tableShapes[]
            uint32_t valueClosure;    // index of function in global list
            uint32_t valueClassShape; // index into classShapes[]
        };
    };

    struct ConstantKey
    {
        Constant::Type type;
        // Note: this stores value* from Constant; when type is Type_Number, this stores the same bits as double does but in uint64_t.
        // For Type_Vectorf, x and y are stored in 'value' and z and w are stored in 'extra1'.
        // For Type_Vectord, x is stored in 'value', y, z and w are stored in 'extra1/2/3' accordingly.
        uint64_t value;
        uint64_t extra1 = 0;
        uint64_t extra2 = 0;
        uint64_t extra3 = 0;

        bool operator==(const ConstantKey& key) const
        {
            return type == key.type && value == key.value && extra1 == key.extra1 && extra2 == key.extra2 && extra3 == key.extra3;
        }
    };

    struct Function
    {
        std::string data;

        uint8_t maxstacksize = 0;
        uint8_t numparams = 0;
        uint8_t numupvalues = 0;
        bool isvararg = false;

        unsigned int debugname = 0;
        int debuglinedefined = 0;

        std::string dump;
        std::string dumpname;
        std::vector<int> dumpinstoffs;
        std::string typeinfo;
    };

    struct DebugLocal
    {
        unsigned int name;

        uint8_t reg;
        uint32_t startpc;
        uint32_t endpc;
    };

    struct DebugUpval
    {
        unsigned int name;
    };

    struct TypedLocal
    {
        LuauBytecodeType type;
        uint8_t reg;
        uint32_t startpc;
        uint32_t endpc;
    };

    struct TypedUpval
    {
        LuauBytecodeType type;
    };

    struct UserdataType
    {
        std::string name;
        uint32_t nameRef = 0;
        bool used = false;
    };

    struct Jump
    {
        uint32_t source;
        uint32_t target;
    };

    struct StringRefHash
    {
        size_t operator()(const StringRef& v) const;
    };

    struct ConstantKeyHash
    {
        size_t operator()(const ConstantKey& key) const;
    };

    struct TableShapeHash
    {
        size_t operator()(const TableShape& v) const;
    };

    std::vector<Function> functions;
    uint32_t currentFunction = ~0u;
    uint32_t mainFunction = ~0u;

    size_t totalInstructionCount = 0;
    std::vector<uint32_t> insns;
    std::vector<int> lines;
    std::vector<Constant> constants;
    std::vector<uint32_t> protos;
    std::vector<Jump> jumps;

    std::vector<TableShape> tableShapes;
    std::vector<ClassShape> classShapes;

    struct FeedbackSlot
    {
        LuauFeedbackType kind;

        union
        {
            struct
            {
                uint32_t pc;
            } callTarget;

            struct
            {
                uint32_t shape;
            } construct;
        };
    };

    std::vector<FeedbackSlot> fbSlots;
    std::vector<uint32_t> fbSlots_DEPRECATED;

    bool hasLongJumps = false;

    DenseHashMap<ConstantKey, int32_t, ConstantKeyHash> constantMap;
    DenseHashMap<TableShape, int32_t, TableShapeHash> tableShapeMap;
    DenseHashMap<uint32_t, int16_t> protoMap;

    int debugLine = 0;

    std::vector<DebugLocal> debugLocals;
    std::vector<DebugUpval> debugUpvals;

    std::vector<TypedLocal> typedLocals;
    std::vector<TypedUpval> typedUpvals;

    std::vector<UserdataType> userdataTypes;

    DenseHashMap<StringRef, unsigned int, StringRefHash> stringTable;
    std::vector<StringRef> debugStrings;

    std::vector<std::pair<uint32_t, uint32_t>> debugRemarks;
    std::string debugRemarkBuffer;

    BytecodeEncoder* encoder = nullptr;
    std::string bytecode;

    uint32_t dumpFlags = 0;
    std::vector<std::string> dumpSource;
    std::vector<std::pair<int, std::string>> dumpRemarks;

    std::string tempTypeInfo;

    std::string (BytecodeBuilder::*dumpFunctionPtr)(std::vector<int>&) const = nullptr;

    void validate() const;
    void validateInstructions() const;
    void validateVariadic() const;
    void validateCaptures() const;
    virtual void validateConst(int32_t cid) const;
    virtual void validateConst(int32_t cid, Constant::Type constType) const;
    virtual uint8_t validateProto(int32_t pid) const;
    virtual uint8_t validateClosure(int32_t cid) const;

    std::string dumpCurrentFunction(std::vector<int>& dumpinstoffs) const;
    virtual void dumpConstant(std::string& result, int k, bool detailed) const;
    void dumpInstruction(const uint32_t* opcode, std::string& result, int targetLabel) const;

    int calcLinesSpan() const;
    void fillBaselineInfo(int span, int* baseline, size_t baselineSize) const;

    void writeFunction(std::string& ss, uint32_t id, uint8_t flags, uint64_t cost);
    void writeLineInfo(std::string& ss) const;
    void writeStringTable(std::string& ss) const;
    void writeClassShape(std::string& ss, const ClassShape& cs) const;

    int32_t addConstant(const ConstantKey& key, const Constant& value);
    unsigned int addStringTableEntry(StringRef value);

    const char* tryGetUserdataTypeName(LuauBytecodeType type) const;
    void clearState();
};

} // namespace Luau
