#pragma once

#include "stream/DspCode.hpp"
#include "green/Memory.hpp"

#include <array>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace openstone::stream::detail {

enum class DspType { Float, Double, Int, Bool, Decimal, Fraction, Hex };
using DspLanes = std::array<std::uint64_t, 4>;
// Cached numeric fields. The complete descriptor and its tables live in DspMemoryArray.
enum class MemArrayField : std::size_t { Count, LastIndex, Size };
enum class MemField : std::size_t { Metadata, Limit, Size };
inline constexpr std::size_t dspNestingLimit = 128;
inline constexpr std::uint32_t nativeFramePort = 0xffffffffU;
inline constexpr std::uint32_t enginePointerPortBase = 0x80000000U;
inline constexpr std::uint32_t engineRootAddress = 64;
inline constexpr std::size_t nativeFramePrefixBytes = 0x1110;
inline constexpr std::size_t dspBackwardBranchLimit = 4096;
inline constexpr std::size_t dspRecursiveCallLimit = 4096;

enum class AssemblyStorage : std::uint64_t { Packet, LocalArray, ArrayInput };

struct DspValue {
    DspType type = DspType::Float;
    DspLanes lanes{};
};

struct DspSelection {
    unsigned mask = 0xf;
    int broadcast = -1;
};

enum class DspOp {
    X87Load, X87Store, X87Copy, X87Exchange, X87Unary, X87Binary,
    Constant, Load, Store, LoadIndexed, LoadArrayInput, StoreIndexed, Duplicate,
    Loop, Repeat, Hop, Select,
    Negate, Complement, Add, Subtract, Multiply, Divide, Remainder, Power,
    Less, LessEqual, Greater, GreaterEqual, Equal, NotEqual, And, Or, Xor,
    ShiftLeft, ShiftRight, ShiftArithmetic, Min, Max, Sin1, Cos1, Tan1,
    Log10, Log2, Ln, Sign, Abs, Exp, Sqrt, RoundNearest, Round, Trunc, Floor,
    Ceil, Convert, Denorm,
    Raw32, Reinterpret128, LoadAssemblyPacket, StoreAssemblyPacket,
    SseMin, SseMax, SseCompare, ShufflePs, ShufflePd, LoadArrayPacket, JumpIfTrue,
    Exp2MinusOne, ScalePowerOfTwo, SseAdd, SseSubtract, SseMultiply, SseDivide, SseSqrt,
    SinRadians, CosRadians, TanRadians, MultiplyUnsignedDoublewords, Jump, ReadTimestamp, ReadSamplePosition, BindNativeFrame,
    IntegerCarry, IntegerParity, IntegerDivide, // IntegerDivide: three inputs -> q/r; bits=8 selects AX/byte, otherwise EDX:EAX/dword.
    PackedShiftCount, LoadAssemblyMemory, StoreAssemblyMemory, LoadAssemblyByte, StoreAssemblyByte, LoadMemByte, StoreMemByte, LoadMemRef, LoadMemArrayField, StoreMemRef, IsMemRefTarget, MemRefPresent, LoadEnginePointer, LocalCall, LocalReturn,
};

constexpr unsigned assemblyElementBytes(DspType type) noexcept { return type == DspType::Double ? 8 : 4; }
constexpr unsigned assemblyPacketLanes(DspType type) noexcept { return 16 / assemblyElementBytes(type); }

constexpr std::uint64_t doubleSignBit = 0x8000000000000000ULL;
constexpr std::uint64_t doubleInfinityBits = 0x7ff0000000000000ULL;
constexpr std::uint64_t doubleQuietNaNBit = 0x0008000000000000ULL;
constexpr std::uint64_t doubleCanonicalNaN = doubleInfinityBits | doubleQuietNaNBit;

struct SseFloatFormat { std::uint64_t sign, infinity, quiet; };
constexpr SseFloatFormat sseFloatFormat(DspType type) noexcept
{
    return type == DspType::Double ? SseFloatFormat{doubleSignBit, doubleInfinityBits, doubleQuietNaNBit}
        : SseFloatFormat{0x80000000U, 0x7f800000U, 0x00400000U};
}
constexpr bool isSseArithmetic(DspOp op) noexcept { return op >= DspOp::SseAdd && op <= DspOp::SseDivide; }

// Beyond this magnitude, scaling a nonzero finite binary64 value must overflow or underflow.
constexpr double scaleExponentLimit = 4096.0;

constexpr bool isLiteral(DspType type) noexcept
{
    return type == DspType::Decimal || type == DspType::Fraction || type == DspType::Hex;
}

constexpr DspType numericFallback(DspType type) noexcept
{
    return type == DspType::Bool ? DspType::Float : type;
}

constexpr DspType commonType(DspType left, DspType right, DspType fallback) noexcept
{
    if (isLiteral(left) && isLiteral(right)) {
        if (fallback == DspType::Int && (left == DspType::Fraction || right == DspType::Fraction)) {
            return DspType::Float;
        }
        return numericFallback(fallback);
    }
    if (isLiteral(right)) {
        std::swap(left, right);
    }
    if (isLiteral(left)) {
        if (right == DspType::Bool) {
            return numericFallback(fallback);
        }
        return left == DspType::Fraction && right == DspType::Int ? DspType::Float : right;
    }
    if (left == right) {
        return left;
    }
    if (left == DspType::Double || right == DspType::Double) {
        return DspType::Double;
    }
    if (left == DspType::Float || right == DspType::Float) {
        return DspType::Float;
    }
    return DspType::Int;
}

constexpr bool isComparison(DspOp op) noexcept { return op >= DspOp::Less && op <= DspOp::NotEqual; }
constexpr bool isBitwise(DspOp op) noexcept { return op >= DspOp::And && op <= DspOp::Xor; }
constexpr bool isShift(DspOp op) noexcept { return op >= DspOp::ShiftLeft && op <= DspOp::ShiftArithmetic; }

constexpr DspType binaryType(DspOp op, DspType left, DspType right, DspType fallback) noexcept
{
    if (isShift(op)) {
        return isLiteral(left) ? DspType::Int : left;
    }
    const auto type = commonType(left, right, fallback);
    return type == DspType::Bool && !isBitwise(op) && !isComparison(op) ? numericFallback(fallback) : type;
}

constexpr DspType unaryType(DspOp op, DspType source, DspType fallback) noexcept
{
    if (op == DspOp::Convert) {
        return fallback;
    }
    const auto type = isLiteral(source) ? numericFallback(fallback) : source;
    if (op != DspOp::Complement && (type == DspType::Bool
            || (type == DspType::Int && op != DspOp::Negate && op != DspOp::Abs && op != DspOp::Sign))) {
        return fallback == DspType::Double ? DspType::Double : DspType::Float;
    }
    return type;
}

struct DspInstruction {
    DspOp op;
    DspType type = DspType::Float;
    std::size_t slot{};
    std::size_t count{}; // Engine memory ops: base GPR slot + 1, or zero for the root; otherwise opcode-specific.
    std::uint64_t bits{};
    DspSelection lanes{};
    DspType storageType = DspType::Float; // Declared element layout for raw SIMD memory accesses.
};

struct DspInitializer {
    std::size_t slot;
    std::size_t count;
    DspType type;
    std::vector<DspLanes> values;
    bool fill = true;
    bool random = false;
    double low{};
    double high{};
};

struct DspNativeProgram;

struct DspProgramData {
    std::array<std::vector<DspInstruction>, 4> stages;
    std::vector<DspInitializer> initializers;
    std::vector<DspPort> inputs;
    std::vector<bool> writableInputs; // Assembler signal-input working slots, refreshed before each stage.
    std::vector<std::string> enginePointers;
    std::vector<bool> enginePointerUsed;
    std::vector<DspPort> outputs;
    std::vector<bool> initializedOutputs; // Assembler stage-0 seeds, separate from the processing stage.
    std::vector<std::size_t> inputSlots;
    std::vector<std::size_t> outputSlots;
    std::array<std::vector<std::size_t>, 4> stageInputs;
    // Canonical four-DWORD register slots, refreshed at every stage invocation.
    std::array<std::vector<std::pair<unsigned, std::size_t>>, 4> incomingXmmSlots;
    std::vector<unsigned> outputStages;
    std::size_t slotCount{};
    std::size_t arraySlotCount{};
    bool hasMemRef{};
    bool hasNativeFrame{};
    std::vector<std::size_t> targetMemoryPorts;
    std::size_t stackSize{};

    [[nodiscard]] bool publishesOutput(std::size_t output, unsigned stage) const noexcept
    {
        return stage == 0 ? output < initializedOutputs.size() && initializedOutputs[output]
                          : outputStages[output] == stage;
    }

    [[nodiscard]] std::shared_ptr<const DspNativeProgram> nativeProgram() const;

private:
    mutable std::once_flag compileNative_;
    mutable std::shared_ptr<const DspNativeProgram> native_;
};

struct DspMemoryRegion {
    std::uint32_t start{};
    green::Memory memory;
    bool writable{};
};

struct DspMemoryArray {
    bool enabled{};
    green::Memory descriptor;
    std::vector<DspMemoryRegion> regions;
};

struct DspEngineRegion {
    std::uint32_t start{};
    green::Memory memory;
    bool writable = true;
    std::vector<std::uint8_t> targets;
};

struct DspEngineCell {
    std::uint32_t start{}, target{};
    std::uintptr_t address{};
    std::uint64_t initialBits{};
    std::size_t region{}, offset{};
    unsigned bytes{};
    bool valid = true;
};

struct DspEngineMemory {
    std::vector<DspEngineRegion> regions;
    std::vector<DspEngineCell> cells; // Sorted by virtual source address.
    std::vector<std::size_t> physicalCells; // Indices sorted by backing byte address.
};

// Private target provenance follows the bound Mem across samples and buffers. No audio-thread allocation.
struct DspMemoryReferences {
    std::vector<green::Memory> memory;
    std::vector<std::vector<std::uint8_t>> targets;
    std::vector<DspMemoryArray> arrays;
    std::vector<DspEngineMemory> engine;
    DspFault fault;
    std::uint32_t samplePosition{}; // Native incoming ECX; distinct from absolute fault/HOP frame.
    std::vector<std::uint8_t> nativeFrame, initialNativeFrame, nativeFrameTargets;
    std::uint64_t nativeFrameStamp{};
    bool nativeFramePrepared{}, nativeFrameContextInitialized{};
    unsigned nativeFrameActive{}, nativeFrameGates{};
    bool nativeFramePoly{};
    std::array<std::size_t, 4> nativeFrameVoiceSlots{};
};

using DspStageEntry = void (*)(DspLanes* slots, const DspLanes* arrays, DspMemoryReferences* references, std::uint64_t frame);

struct DspNativeProgram {
    std::array<DspStageEntry, 4> stages{};
    std::shared_ptr<void> lifetime;
    std::string optimizedIr;
};

[[nodiscard]] std::shared_ptr<const DspNativeProgram> compileDspOrc(const DspProgramData& program);

[[nodiscard]] DspType dspPortType(DspPortType port) noexcept;
[[nodiscard]] std::uint64_t dspEncode(double number, DspType type) noexcept;
[[nodiscard]] double dspDecode(std::uint64_t bits, DspType type) noexcept;
[[nodiscard]] std::uint64_t dspConvert(std::uint64_t bits, DspType from, DspType to) noexcept;
// An x87 scalar occupies a scratch record: significand in word 0, sign/exponent in word 1.
// Scratch is written before use and the x87 stack is empty at each stage boundary.
void dspX87Load(DspLanes* destination, std::uint64_t bits, std::uint64_t type) noexcept;
[[nodiscard]] std::uint64_t dspX87Store(const DspLanes* source, std::uint64_t type) noexcept;
void dspX87Unary(DspLanes* slot, std::uint64_t operation) noexcept;
void dspX87Binary(DspLanes* destination, const DspLanes* source, std::uint64_t operation) noexcept;

void dspReportDivideFault(DspMemoryReferences* references, std::uint64_t kind, std::uint64_t frame) noexcept;
[[nodiscard]] std::uint64_t dspTimestamp() noexcept;
[[nodiscard]] std::uint64_t dspSamplePosition(const DspMemoryReferences* memory) noexcept;
void prepareDspNativeFrame(DspMemoryReferences& memory, std::uint64_t frame, bool poly,
                          unsigned active, unsigned gates, const std::array<std::size_t, 4>& voiceSlots) noexcept;
[[nodiscard]] std::uint64_t dspReadMemRef(const DspMemoryReferences* memory, std::uint64_t port, std::int64_t offset, unsigned bytes) noexcept;
[[nodiscard]] std::uint64_t dspReadMemArrayField(const DspMemoryReferences* memory, std::uint64_t port, std::int64_t offset, unsigned bytes) noexcept;
[[nodiscard]] DspMemoryArray bindDspMemArray(const green::MemoryArray& values);
void dspWriteMemRef(DspMemoryReferences* memory, std::uint64_t port, std::int64_t offset, unsigned bytes,
                    std::uint64_t value, unsigned target) noexcept;
[[nodiscard]] std::uint64_t dspLoadEnginePointer(const DspMemoryReferences* memory, std::uint64_t port, std::int64_t base, std::int64_t offset) noexcept;
[[nodiscard]] std::int64_t dspEngineOffset(const DspMemoryReferences* memory, std::uint64_t port, std::int64_t base, std::int64_t offset, unsigned bytes) noexcept;
[[nodiscard]] std::uint64_t dspMemRefPresent(const DspMemoryReferences* memory, std::uint64_t port) noexcept;
[[nodiscard]] std::uint64_t dspIsMemRefTarget(const DspMemoryReferences* memory, std::uint64_t port, std::int64_t offset) noexcept;

void initializeDspSlots(std::span<const DspInitializer> initializers, std::span<DspLanes> slots) noexcept;
void runDspStage(std::span<const DspInstruction> code, std::span<DspLanes> slots,
                 std::span<const DspLanes> arrays, DspMemoryReferences& references,
                 std::span<DspValue> stack, std::uint64_t frame) noexcept;

}
