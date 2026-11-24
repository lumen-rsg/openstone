#include "stream/DspProgramData.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <type_traits>

namespace openstone::stream::detail {
namespace {

constexpr std::uint64_t mask(bool value) noexcept
{
    return value ? ~std::uint64_t{} : 0;
}

constexpr std::uint32_t lowBits(std::uint64_t bits) noexcept
{
    return static_cast<std::uint32_t>(bits);
}

constexpr std::int32_t signedInt(std::uint64_t bits) noexcept
{
    return std::bit_cast<std::int32_t>(lowBits(bits));
}

std::uint64_t bitOperand(std::uint64_t bits, DspType from, DspType to) noexcept
{
    return from == DspType::Bool ? bits : dspConvert(bits, from, to);
}

std::uint64_t shifted(DspOp op, std::uint64_t left, double count, unsigned width) noexcept
{
    const bool negative = (left >> (width - 1)) != 0;
    if (!(count >= 0 && count < width)) {
        return op == DspOp::ShiftArithmetic && negative ? mask(true) : 0;
    }
    const auto shift = static_cast<unsigned>(count);
    if (op == DspOp::ShiftLeft) {
        return left << shift;
    }
    if (op == DspOp::ShiftArithmetic && negative && shift != 0) {
        return (left >> shift) | (mask(true) << (width - shift));
    }
    return left >> shift;
}

bool comparison(DspOp op, double a, double b) noexcept
{
    switch (op) {
    case DspOp::Less: return a < b;
    case DspOp::LessEqual: return a <= b;
    case DspOp::Greater: return a > b;
    case DspOp::GreaterEqual: return a >= b;
    case DspOp::Equal: return a == b;
    case DspOp::NotEqual: return a != b;
    default: std::abort();
    }
}

template<typename Float>
Float floatingBinary(DspOp op, Float a, Float b) noexcept
{
    switch (op) {
    case DspOp::Add: return a + b;
    case DspOp::Subtract: return a - b;
    case DspOp::Multiply: return a * b;
    case DspOp::Divide: return a / b;
    case DspOp::Remainder: return std::fmod(a, b);
    case DspOp::Power: return std::pow(a, b);
    case DspOp::Min: return std::fmin(a, b);
    case DspOp::Max: return std::fmax(a, b);
    case DspOp::ScalePowerOfTwo:
        if (!std::isfinite(b)) return a * std::exp2(b);
        return std::ldexp(a, static_cast<int>(std::clamp(static_cast<double>(b),
                                                        -scaleExponentLimit, scaleExponentLimit)));
    default: std::abort();
    }
}

std::uint64_t integerBinary(DspOp op, std::uint64_t left, std::uint64_t right) noexcept
{
    const auto a = signedInt(left);
    const auto b = signedInt(right);
    const auto ua = lowBits(left);
    const auto ub = lowBits(right);
    switch (op) {
    case DspOp::Add: return ua + ub;
    case DspOp::Subtract: return ua - ub;
    case DspOp::Multiply: return ua * ub;
    case DspOp::Divide:
        return b == 0 ? 0 : static_cast<std::uint32_t>(static_cast<std::int64_t>(a) / b);
    case DspOp::Remainder:
        return b == 0 ? 0 : static_cast<std::uint32_t>(static_cast<std::int64_t>(a) % b);
    case DspOp::Power: return dspEncode(std::pow(static_cast<double>(a), static_cast<double>(b)), DspType::Int);
    case DspOp::Min: return static_cast<std::uint32_t>(std::min(a, b));
    case DspOp::Max: return static_cast<std::uint32_t>(std::max(a, b));
    default: std::abort();
    }
}

bool sseCompare(double a, double b, std::uint64_t predicate) noexcept
{
    switch (predicate) {
    case 0: return a == b;
    case 1: return a < b;
    case 2: return a <= b;
    case 3: return std::isunordered(a, b);
    case 4: return a != b;
    case 5: return !(a < b);
    case 6: return !(a <= b);
    case 7: return !std::isunordered(a, b);
    default: std::abort();
    }
}

constexpr bool isSseNaN(std::uint64_t bits, DspType type) noexcept
{
    const auto format = sseFloatFormat(type);
    const auto magnitude = type == DspType::Double ? bits & ~format.sign : lowBits(bits) & ~format.sign;
    return magnitude > format.infinity;
}

template<typename Float>
std::uint64_t sseArithmetic(DspOp op, std::uint64_t left, std::uint64_t right) noexcept
{
    constexpr auto type = sizeof(Float) == 8 ? DspType::Double : DspType::Float;
    const auto format = sseFloatFormat(type);
    const bool leftNaN = isSseNaN(left, type), rightNaN = isSseNaN(right, type);
    // The saved FlowStone Windows ARM traces prioritize a signaling NaN,
    // then the left NaN. Quieting retains its sign and payload.
    if (rightNaN && (!leftNaN || ((right & format.quiet) == 0 && (left & format.quiet) != 0))) return right | format.quiet;
    if (leftNaN) return left | format.quiet;
    using Word = std::conditional_t<sizeof(Float) == 8, std::uint64_t, std::uint32_t>;
    const auto a = std::bit_cast<Float>(static_cast<Word>(left));
    const auto b = std::bit_cast<Float>(static_cast<Word>(right));
    Float value;
    switch (op) {
    case DspOp::SseAdd: value = a + b; break;
    case DspOp::SseSubtract: value = a - b; break;
    case DspOp::SseMultiply: value = a * b; break;
    case DspOp::SseDivide: value = a / b; break;
    default: std::abort();
    }
    const auto result = std::bit_cast<Word>(value);
    return isSseNaN(result, type) ? format.infinity | format.quiet : result;
}

template<typename Float>
std::uint64_t sseSqrt(std::uint64_t bits) noexcept
{
    constexpr auto type = sizeof(Float) == 8 ? DspType::Double : DspType::Float;
    const auto format = sseFloatFormat(type);
    if (isSseNaN(bits, type)) return bits | format.quiet;
    using Word = std::conditional_t<sizeof(Float) == 8, std::uint64_t, std::uint32_t>;
    const auto source = std::bit_cast<Float>(static_cast<Word>(bits));
    if (source < 0) return format.sign | format.infinity | format.quiet;
    return std::bit_cast<Word>(std::sqrt(source));
}

void applySse(const DspInstruction& op, DspValue& left, const DspValue& right) noexcept
{
    const auto before = left.lanes;
    if (op.op == DspOp::ShufflePd) {
        left.lanes = {before[op.bits & 1U], right.lanes[(op.bits >> 1) & 1U], 0, 0};
        left.type = DspType::Double;
        return;
    }
    if (op.op == DspOp::MultiplyUnsignedDoublewords) {
        for (std::size_t lane = 0; lane < before.size(); lane += 2) {
            const auto product = std::uint64_t{lowBits(before[lane])} * lowBits(right.lanes[lane]);
            left.lanes[lane] = lowBits(product);
            left.lanes[lane + 1] = product >> 32;
        }
        left.type = DspType::Int;
        return;
    }
    for (unsigned lane = 0; lane < before.size(); ++lane) {
        if (op.op == DspOp::ShufflePs) {
            const auto source = (op.bits >> (2 * lane)) & 3U;
            left.lanes[lane] = (lane < 2 ? before : right.lanes)[source];
            continue;
        }
        if (isSseArithmetic(op.op)) {
            left.lanes[lane] = op.type == DspType::Double ? sseArithmetic<double>(op.op, before[lane], right.lanes[lane])
                : sseArithmetic<float>(op.op, before[lane], right.lanes[lane]);
            continue;
        }
        const auto a = dspDecode(before[lane], op.type);
        const auto b = dspDecode(right.lanes[lane], op.type);
        if (op.op == DspOp::SseCompare) {
            const auto bits = mask(sseCompare(a, b, op.bits));
            left.lanes[lane] = op.type == DspType::Double ? bits : lowBits(bits);
        } else {
            // SSE minimum and maximum select the second operand on ties and NaNs.
            const bool selectLeft = op.op == DspOp::SseMin ? a < b : a > b;
            left.lanes[lane] = selectLeft ? before[lane] : right.lanes[lane];
        }
    }
    left.type = op.type;
}

void applyBinary(DspOp op, DspType fallback, DspValue& left, const DspValue& right) noexcept
{
    const auto type = binaryType(op, left.type, right.type, fallback);
    const bool compare = isComparison(op);
    const bool bitwise = isBitwise(op);
    const bool shift = isShift(op);

    for (std::size_t lane = 0; lane < left.lanes.size(); ++lane) {
        const auto a = left.lanes[lane];
        const auto b = right.lanes[lane];
        if (bitwise || shift) {
            const auto lhs = bitOperand(a, left.type, type);
            const auto rhs = bitOperand(b, right.type, type);
            switch (op) {
            case DspOp::And: left.lanes[lane] = lhs & rhs; break;
            case DspOp::Or: left.lanes[lane] = lhs | rhs; break;
            case DspOp::Xor: left.lanes[lane] = lhs ^ rhs; break;
            default:
                left.lanes[lane] = shifted(op, lhs, dspDecode(b, right.type),
                    type == DspType::Double || type == DspType::Bool ? 64 : 32);
                break;
            }
            if (type == DspType::Float || type == DspType::Int) {
                left.lanes[lane] = lowBits(left.lanes[lane]);
            }
            continue;
        }

        const auto lhs = dspConvert(a, left.type, type);
        const auto rhs = dspConvert(b, right.type, type);
        const auto x = dspDecode(lhs, type);
        const auto y = dspDecode(rhs, type);
        if (compare) {
            left.lanes[lane] = mask(comparison(op, x, y));
        } else if (type == DspType::Int) {
            left.lanes[lane] = integerBinary(op, lhs, rhs);
        } else if (type == DspType::Double) {
            left.lanes[lane] = dspEncode(floatingBinary(op, x, y), type);
        } else {
            left.lanes[lane] = dspEncode(floatingBinary(op, static_cast<float>(x), static_cast<float>(y)), type);
        }
    }
    left.type = compare ? DspType::Bool : type;
}

template<typename Float>
Float floatingUnary(DspOp op, Float x) noexcept
{
    constexpr auto turn = Float{2} * std::numbers::pi_v<Float>;
    switch (op) {
    case DspOp::Negate: return -x;
    case DspOp::SinRadians: return std::sin(x);
    case DspOp::CosRadians: return std::cos(x);
    case DspOp::TanRadians: return std::tan(x);
    case DspOp::Sin1: return std::sin(turn * x);
    case DspOp::Cos1: return std::cos(turn * x);
    case DspOp::Tan1: return std::tan(turn * x);
    case DspOp::Log10: return std::log10(x);
    case DspOp::Log2: return std::log2(x);
    case DspOp::Ln: return std::log(x);
    case DspOp::Sign: return static_cast<Float>((x > 0) - (x < 0));
    case DspOp::Abs: return std::abs(x);
    case DspOp::Exp: return std::exp(x);
    case DspOp::Exp2MinusOne: return std::expm1(x * std::numbers::ln2_v<Float>);
    case DspOp::Sqrt: return std::sqrt(x);
    case DspOp::RoundNearest: return std::nearbyint(x);
    case DspOp::Round: return std::round(x);
    case DspOp::Trunc: return std::trunc(x);
    case DspOp::Floor: return std::floor(x);
    case DspOp::Ceil: return std::ceil(x);
    case DspOp::Denorm: return std::fpclassify(x) == FP_SUBNORMAL ? std::copysign(Float{}, x) : x;
    default: std::abort();
    }
}

void applyUnary(DspOp op, DspType fallback, DspValue& value) noexcept
{
    if (op == DspOp::Negate && (value.type == DspType::Decimal || value.type == DspType::Fraction)) {
        for (auto& bits : value.lanes) {
            bits ^= std::uint64_t{1} << 63;
        }
        return;
    }
    const auto type = unaryType(op, value.type, fallback);
    for (auto& bits : value.lanes) {
        bits = dspConvert(bits, value.type, type);
        if (op == DspOp::Convert) {
            continue;
        }
        if (op == DspOp::Complement) {
            bits = type == DspType::Double || type == DspType::Bool ? ~bits : lowBits(~bits);
        } else if (op == DspOp::Denorm) {
            // Classify raw bits: a float-to-double conversion would quiet an SNaN
            // before the following SSE instruction can select its payload.
            const auto format = sseFloatFormat(type);
            const auto magnitude = bits & ~format.sign;
            const auto minimum = type == DspType::Double ? 0x0010000000000000ULL : 0x00800000ULL;
            if (magnitude != 0 && magnitude < minimum) bits &= format.sign;
        } else if (op == DspOp::SseSqrt) {
            bits = type == DspType::Double ? sseSqrt<double>(bits) : sseSqrt<float>(bits);
        } else if (type == DspType::Int) {
            const auto integer = signedInt(bits);
            if (op == DspOp::Negate || (op == DspOp::Abs && integer < 0)) {
                bits = std::uint32_t{0} - lowBits(bits);
            } else if (op == DspOp::Sign) {
                bits = static_cast<std::uint32_t>((integer > 0) - (integer < 0));
            }
        } else {
            const auto x = dspDecode(bits, type);
            bits = dspEncode(type == DspType::Double
                ? floatingUnary(op, x) : floatingUnary(op, static_cast<float>(x)), type);
        }
    }
    value.type = type;
}

constexpr bool includes(DspSelection selection, std::size_t lane) noexcept
{
    return (selection.mask & (1U << lane)) != 0;
}

DspLanes selectLanes(DspLanes values, DspSelection selection) noexcept
{
    if (selection.broadcast >= 0) {
        values.fill(values[static_cast<std::size_t>(selection.broadcast)]);
    }
    for (std::size_t lane = 0; lane < values.size(); ++lane) {
        if (!includes(selection, lane)) {
            values[lane] = 0;
        }
    }
    return values;
}

std::size_t arrayIndex(const DspValue& index, std::size_t lane, std::size_t count) noexcept
{
    const auto value = dspDecode(index.lanes[lane], index.type);
    return value >= 0 && value < static_cast<double>(count) ? static_cast<std::size_t>(value) : count;
}

DspValue loadIndexed(std::span<const DspLanes> memory, const DspValue& index, const DspInstruction& op) noexcept
{
    DspValue values{op.type};
    for (std::size_t lane = 0; lane < values.lanes.size(); ++lane) {
        const auto offset = arrayIndex(index, lane, op.count);
        if (offset < op.count) {
            values.lanes[lane] = memory[op.slot + offset][lane];
        }
    }
    values.lanes = selectLanes(values.lanes, op.lanes);
    return values;
}

std::size_t assemblyElement(const DspValue& index, const DspInstruction& op) noexcept
{
    const auto bytes = static_cast<std::int32_t>(assemblyElementBytes(op.type));
    const auto offset = signedInt(index.lanes[0]);
    return offset >= 0 && offset % bytes == 0 ? static_cast<std::size_t>(offset / bytes) : op.count;
}

DspValue loadAssemblyMemory(std::span<const DspLanes> memory, const DspValue& index, const DspInstruction& op) noexcept
{
    DspValue result{op.type};
    const auto first = assemblyElement(index, op);
    const bool packet = op.bits == static_cast<std::uint64_t>(AssemblyStorage::Packet);
    for (unsigned lane = 0; lane < assemblyPacketLanes(op.type) && first + lane < op.count; ++lane) {
        const auto element = first + lane;
        result.lanes[lane] = packet ? memory[op.slot][element] : memory[op.slot + element][0];
    }
    result.lanes = selectLanes(result.lanes, op.lanes);
    return result;
}

void reinterpret128(DspValue& value, DspType type) noexcept
{
    const auto source = value.lanes;
    if (type == DspType::Double) {
        value.lanes = value.type == DspType::Double ? DspLanes{source[0], source[1], 0, 0}
            : DspLanes{std::uint64_t{lowBits(source[0])} | (std::uint64_t{lowBits(source[1])} << 32),
                       std::uint64_t{lowBits(source[2])} | (std::uint64_t{lowBits(source[3])} << 32), 0, 0};
    } else if (value.type == DspType::Double) {
        value.lanes = {lowBits(source[0]), lowBits(source[0] >> 32), lowBits(source[1]), lowBits(source[1] >> 32)};
    } else {
        for (auto& lane : value.lanes) lane = lowBits(lane);
    }
    value.type = type;
}

DspValue loadAssemblyPacket(std::span<const DspLanes> memory, const DspValue& index, const DspInstruction& op) noexcept
{
    DspValue result{op.type};
    const auto offset = static_cast<std::int64_t>(signedInt(index.lanes[0]));
    const auto storageBytes = assemblyElementBytes(op.storageType);
    const auto bytes = assemblyElementBytes(op.type);
    const bool packet = op.bits == static_cast<std::uint64_t>(AssemblyStorage::Packet);
    if (offset < 0 || offset % 4 != 0) return result;
    for (unsigned lane = 0; lane < assemblyPacketLanes(op.type); ++lane) {
        const auto position = static_cast<std::uint64_t>(offset) + lane * bytes;
        if (position + bytes > op.count * storageBytes) continue;
        for (unsigned word = 0; word < bytes / 4; ++word) {
            const auto at = position + word * 4;
            const auto element = at / storageBytes;
            const auto bits = packet ? memory[op.slot][element] : memory[op.slot + element][0];
            const auto piece = lowBits(bits >> ((at % storageBytes) * 8));
            result.lanes[lane] |= std::uint64_t{piece} << (word * 32);
        }
    }
    result.lanes = selectLanes(result.lanes, op.lanes);
    return result;
}

void storeAssemblyPacket(std::span<DspLanes> memory, const DspValue& value, const DspValue& index,
                         const DspInstruction& op) noexcept
{
    const auto offset = static_cast<std::int64_t>(signedInt(index.lanes[0]));
    const auto storageBytes = assemblyElementBytes(op.storageType);
    const auto bytes = assemblyElementBytes(op.type);
    const bool packet = op.bits == static_cast<std::uint64_t>(AssemblyStorage::Packet);
    if (offset < 0 || offset % 4 != 0) return;
    for (unsigned lane = 0; lane < assemblyPacketLanes(op.type); ++lane) {
        if (!includes(op.lanes, lane)) continue;
        const auto position = static_cast<std::uint64_t>(offset) + lane * bytes;
        if (position + bytes > op.count * storageBytes) continue;
        const auto source = op.lanes.broadcast < 0 ? lane : static_cast<unsigned>(op.lanes.broadcast);
        for (unsigned word = 0; word < bytes / 4; ++word) {
            const auto at = position + word * 4;
            const auto element = at / storageBytes;
            auto& target = packet ? memory[op.slot][element] : memory[op.slot + element][0];
            const auto shift = (at % storageBytes) * 8;
            const auto piece = std::uint64_t{lowBits(value.lanes[source] >> (word * 32))};
            target = storageBytes == 4 ? piece : (target & ~(std::uint64_t{UINT32_MAX} << shift)) | (piece << shift);
        }
    }
}

void storeAssemblyMemory(std::span<DspLanes> slots, const DspValue& value, const DspValue& index,
                         const DspInstruction& op) noexcept
{
    const auto first = assemblyElement(index, op);
    const bool packet = op.bits == static_cast<std::uint64_t>(AssemblyStorage::Packet);
    for (unsigned lane = 0; lane < assemblyPacketLanes(op.type) && first + lane < op.count; ++lane) {
        if (!includes(op.lanes, lane)) continue;
        const auto element = first + lane;
        auto& target = packet ? slots[op.slot][element] : slots[op.slot + element][0];
        target = dspConvert(value.lanes[lane], value.type, op.type);
    }
}

void store(std::span<DspLanes> slots, const DspValue& value, const DspInstruction& op,
           const DspValue* index = nullptr) noexcept
{
    for (std::size_t lane = 0; lane < value.lanes.size(); ++lane) {
        if (!includes(op.lanes, lane)) {
            continue;
        }
        const auto offset = index ? arrayIndex(*index, lane, op.count) : 0;
        if (!index || offset < op.count) {
            const auto source = op.lanes.broadcast < 0 ? lane : static_cast<std::size_t>(op.lanes.broadcast);
            slots[op.slot + offset][lane] = dspConvert(value.lanes[source], value.type, op.type);
        }
    }
}

void selectValue(DspValue& condition, const DspValue& yes, const DspValue& no, DspType fallback) noexcept
{
    const auto result = commonType(yes.type, no.type, fallback);
    for (std::size_t lane = 0; lane < condition.lanes.size(); ++lane) {
        const auto& chosen = dspDecode(condition.lanes[lane], condition.type) != 0 ? yes : no;
        condition.lanes[lane] = dspConvert(chosen.lanes[lane], chosen.type, result);
    }
    condition.type = result;
}

double randomUnit(std::uint64_t& state) noexcept
{
    // SplitMix64 produces a reproducible initialization on every supported platform.
    auto z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return static_cast<double>((z ^ (z >> 31)) >> 11) * 0x1p-53;
}

}

DspType dspPortType(DspPortType port) noexcept
{
    switch (port) {
    case DspPortType::StreamDouble: case DspPortType::MonoDouble: case DspPortType::PolyDouble:
        return DspType::Double;
    case DspPortType::StreamBoolean: case DspPortType::MonoBoolean: case DspPortType::PolyBoolean:
        return DspType::Bool;
    case DspPortType::PolyInt:
        return DspType::Int;
    default:
        return DspType::Float;
    }
}

std::uint64_t dspEncode(double number, DspType type) noexcept
{
    switch (type) {
    case DspType::Double: case DspType::Decimal: case DspType::Fraction:
        return std::bit_cast<std::uint64_t>(number);
    case DspType::Float: {
        constexpr auto max = std::numeric_limits<float>::max();
        if (std::abs(number) > max) {
            const auto magnitude = std::abs(number) < 0x1.ffffffp127
                ? max : std::numeric_limits<float>::infinity();
            return std::bit_cast<std::uint32_t>(std::copysign(magnitude, std::signbit(number) ? -1.0F : 1.0F));
        }
        return std::bit_cast<std::uint32_t>(static_cast<float>(number));
    }
    case DspType::Bool:
        return mask(number != 0);
    case DspType::Int: {
        const auto integral = std::trunc(number);
        if (!(integral >= -2147483648.0 && integral < 2147483648.0)) {
            return 0x80000000U;
        }
        return static_cast<std::uint32_t>(static_cast<std::int32_t>(integral));
    }
    case DspType::Hex:
        std::abort();
    }
    std::abort();
}

double dspDecode(std::uint64_t bits, DspType type) noexcept
{
    switch (type) {
    case DspType::Double: case DspType::Decimal: case DspType::Fraction: return std::bit_cast<double>(bits);
    case DspType::Float: return std::bit_cast<float>(lowBits(bits));
    case DspType::Int: return signedInt(bits);
    case DspType::Bool: return bits != 0 ? 1 : 0;
    case DspType::Hex: return static_cast<double>(bits);
    }
    std::abort();
}

std::uint64_t dspConvert(std::uint64_t bits, DspType from, DspType to) noexcept
{
    if (from == to) {
        return bits;
    }
    if (from == DspType::Hex) {
        if (to == DspType::Bool) {
            return mask(bits != 0);
        }
        return to == DspType::Double ? bits : lowBits(bits);
    }
    return dspEncode(dspDecode(bits, from), to);
}

void initializeDspSlots(std::span<const DspInitializer> initializers, std::span<DspLanes> slots) noexcept
{
    std::uint64_t randomState{};
    for (const auto& init : initializers) {
        auto target = slots.subspan(init.slot, init.count);
        if (init.random) {
            for (auto& values : target) {
                for (auto& lane : values) {
                    const auto position = randomUnit(randomState);
                    const auto x = std::lerp(init.low, init.high, position);
                    lane = dspEncode(init.type == DspType::Int ? std::floor(x) : x, init.type);
                }
            }
        } else if (init.fill) {
            std::ranges::fill(target, init.values.front());
        } else {
            std::ranges::copy(init.values, target.begin());
        }
    }
}

void prepareDspNativeFrame(DspMemoryReferences& memory, std::uint64_t frame, bool poly,
                          unsigned active, unsigned gates, const std::array<std::size_t, 4>& voiceSlots) noexcept
{
    if (memory.nativeFrame.size() < nativeFramePrefixBytes || (memory.nativeFramePrepared && memory.nativeFrameStamp == frame)) return;
    memory.nativeFrameStamp = frame;
    memory.nativeFramePrepared = true;
    const auto word = [&](std::size_t offset, std::uint32_t value) {
        auto& tags = memory.nativeFrameTargets;
        if (!tags.empty()) std::fill(tags.begin() + std::min(tags.size(), offset - 3), tags.begin() + std::min(tags.size(), offset + 4), 0);
        for (unsigned i = 0; i < 4; ++i) memory.nativeFrame[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    for (unsigned lane = 0; lane < 4; ++lane) {
        const auto bit = 1U << lane;
        if (!memory.nativeFrameContextInitialized || ((memory.nativeFrameGates ^ gates) & bit))
            word(0x1010 + lane * 4, (gates & bit) ? 0xffffffffU : 0);
        if (!memory.nativeFrameContextInitialized || ((memory.nativeFrameActive ^ active) & bit))
            word(0x1050 + lane * 4, (active & bit) ? 0xffffffffU : 0);
        if (!memory.nativeFrameContextInitialized || memory.nativeFramePoly != poly)
            word(0x1080 + lane * 4, poly ? 0xffffffffU : 0);
        if (poly && (active & bit) && (!memory.nativeFrameContextInitialized || !(memory.nativeFrameActive & bit) || memory.nativeFrameVoiceSlots[lane] != voiceSlots[lane]))
            word(0x10c0 + lane * 4, std::bit_cast<std::uint32_t>(static_cast<float>(voiceSlots[lane])));
    }
    memory.nativeFrameContextInitialized = true;
    memory.nativeFrameActive = active;
    memory.nativeFrameGates = gates;
    memory.nativeFramePoly = poly;
    memory.nativeFrameVoiceSlots = voiceSlots;
    // The native sample loop writes only the low Float of this packet.
    word(0x1040, std::bit_cast<std::uint32_t>(static_cast<float>(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(frame)))));
}

std::uint64_t dspSamplePosition(const DspMemoryReferences* memory) noexcept
{
    return memory->samplePosition;
}

std::uint64_t dspTimestamp() noexcept
{
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

namespace {
std::uint64_t readMemory(const green::Memory& memory, std::int64_t offset, unsigned bytes) noexcept
{
    if (offset < 0 || static_cast<std::uint64_t>(offset) > memory.size() ||
        memory.size() - static_cast<std::size_t>(offset) < bytes) return 0;
    const auto first = static_cast<std::size_t>(offset);
    if (bytes == 1) return memory.byte(first);
    const auto low = memory.wordAt(first);
    return bytes == 4 ? low : low | (static_cast<std::uint64_t>(memory.wordAt(first + 4)) << 32);
}

template<typename Engine>
auto engineRegion(Engine& engine, std::int64_t offset, unsigned bytes) noexcept -> decltype(&engine.regions.front())
{
    if (offset < engineRootAddress || offset > INT32_MAX) return nullptr;
    const auto next = std::upper_bound(engine.regions.begin(), engine.regions.end(), offset,
        [](std::int64_t address, const DspEngineRegion& region) { return address < region.start; });
    if (next == engine.regions.begin()) return nullptr;
    auto& region = *std::prev(next);
    const auto local = static_cast<std::uint64_t>(offset) - region.start;
    return local <= region.memory.size() && bytes <= region.memory.size() - local ? &region : nullptr;
}

auto engineCellsAt(const DspEngineMemory& engine, std::uintptr_t address) noexcept
{
    return std::lower_bound(engine.physicalCells.begin(), engine.physicalCells.end(), address,
        [&](std::size_t cell, std::uintptr_t first) { return engine.cells[cell].address + engine.cells[cell].bytes <= first; });
}

bool enginePointerBytes(const DspMemoryReferences& references, const green::Memory& memory, std::int64_t offset, unsigned bytes) noexcept
{
    if (offset < 0 || static_cast<std::uint64_t>(offset) > memory.size() || bytes > memory.size() - static_cast<std::size_t>(offset)) return false;
    const auto address = reinterpret_cast<std::uintptr_t>(memory.address()) + static_cast<std::size_t>(offset);
    for (const auto& engine : references.engine) {
        const auto cell = engineCellsAt(engine, address);
        if (cell != engine.physicalCells.end() && engine.cells[*cell].address < address + bytes) return true;
    }
    return false;
}

std::uint64_t readBoundMemory(const DspMemoryReferences& references, const green::Memory& memory, std::int64_t offset, unsigned bytes) noexcept
{
    return enginePointerBytes(references, memory, offset, bytes) ? 0 : readMemory(memory, offset, bytes);
}

const DspMemoryRegion* arrayRegion(const DspMemoryArray& array, std::int64_t offset, unsigned bytes) noexcept
{
    if (offset < 64 || offset > INT32_MAX) return nullptr;
    const auto next = std::upper_bound(array.regions.begin(), array.regions.end(), offset,
        [](std::int64_t address, const DspMemoryRegion& region) { return address < region.start; });
    if (next == array.regions.begin()) return nullptr;
    const auto& region = *std::prev(next);
    const auto local = static_cast<std::uint64_t>(offset) - region.start;
    return local <= region.memory.size() && bytes <= region.memory.size() - local ? &region : nullptr;
}
}

DspMemoryArray bindDspMemArray(const green::MemoryArray& values)
{
    DspMemoryArray binding{.enabled = true, .descriptor = green::Memory(std::vector<std::uint8_t>(64))};
    const auto entries = values.entries();
    if (entries.size() > static_cast<std::size_t>(INT32_MAX) / 24) throw std::length_error("MemArray descriptor is too large");
    const auto count = entries.size();
    for (unsigned i = 0; i < 4; ++i) binding.descriptor.writeWordAt(i * 4, static_cast<std::uint32_t>(count));
    const auto limit = std::bit_cast<std::uint32_t>(static_cast<float>(count == 0 ? 0 : count - 1));
    for (unsigned i = 4; i < 8; ++i) binding.descriptor.writeWordAt(i * 4, limit);
    if (count == 0) return binding;
    const auto align = [](std::size_t n) { return (n + 7) & ~std::size_t(7); };
    std::size_t cursor = 64;
    const auto table = [&](unsigned field, std::size_t stride) {
        const auto index = binding.regions.size();
        binding.descriptor.writeWordAt(field * 8, static_cast<std::uint32_t>(cursor));
        binding.regions.push_back({static_cast<std::uint32_t>(cursor), green::Memory(std::vector<std::uint8_t>(count * stride)), false});
        cursor = align(cursor + count * stride);
        return index;
    };
    const auto channels = table(4, 4), addresses = table(5, 8), sizes = table(6, 4), limits = table(7, 4);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& memory = entries[i];
        const auto frames = memory.channels() == 0 ? 0 : memory.size() / (sizeof(float) * memory.channels());
        if (cursor > INT32_MAX || memory.size() > static_cast<std::size_t>(INT32_MAX) - cursor) {
            throw std::length_error("MemArray exceeds its virtual address space");
        }
        binding.regions[channels].memory.writeWordAt(i * 4, static_cast<std::uint32_t>(memory.channels()));
        binding.regions[sizes].memory.writeWordAt(i * 4, static_cast<std::uint32_t>(memory.size()));
        binding.regions[limits].memory.writeWordAt(i * 4, std::bit_cast<std::uint32_t>(static_cast<float>(frames == 0 ? 0 : frames - 1)));
        if (memory.size() != 0) {
            binding.regions[addresses].memory.writeWordAt(i * 8, static_cast<std::uint32_t>(cursor));
            binding.regions.push_back({static_cast<std::uint32_t>(cursor), memory, true});
            cursor = align(cursor + memory.size());
        }
    }
    return binding;
}

std::uint64_t dspReadMemArrayField(const DspMemoryReferences* references, std::uint64_t port,
                                  std::int64_t offset, unsigned bytes) noexcept
{
    return readMemory(references->arrays[port].descriptor, offset, bytes);
}

std::uint64_t dspReadMemRef(const DspMemoryReferences* references, std::uint64_t port, std::int64_t offset, unsigned bytes) noexcept
{
    if (port == nativeFramePort) {
        const auto& data = references->nativeFrame;
        if (offset < 0 || static_cast<std::uint64_t>(offset) > data.size() || data.size() - static_cast<std::size_t>(offset) < bytes) return 0;
        std::uint64_t value = 0;
        for (unsigned i = 0; i < bytes; ++i) value |= std::uint64_t{data[static_cast<std::size_t>(offset) + i]} << (8 * i);
        return value;
    }
    if (port >= enginePointerPortBase) {
        const auto binding = port - enginePointerPortBase;
        if (binding >= references->engine.size()) return 0;
        const auto* region = engineRegion(references->engine[binding], offset, bytes);
        return region ? readBoundMemory(*references, region->memory, offset - region->start, bytes) : 0;
    }
    if (references->arrays[port].enabled) {
        const auto& array = references->arrays[port];
        const auto* region = arrayRegion(array, offset, bytes);
        return region ? readBoundMemory(*references, region->memory, offset - region->start, bytes) : 0;
    }
    return readBoundMemory(*references, references->memory[port], offset, bytes);
}

void dspWriteMemRef(DspMemoryReferences* references, std::uint64_t port, std::int64_t offset,
                    unsigned bytes, std::uint64_t value, unsigned target) noexcept
{
    if (port == nativeFramePort) {
        auto& data = references->nativeFrame;
        if (offset < 0 || static_cast<std::uint64_t>(offset) > data.size() || data.size() - static_cast<std::size_t>(offset) < bytes) return;
        const auto first = static_cast<std::size_t>(offset);
        auto& tags = references->nativeFrameTargets;
        const auto begin = first > 3 ? first - 3 : 0;
        if (!tags.empty()) std::fill(tags.begin() + std::min(tags.size(), begin), tags.begin() + std::min(tags.size(), first + bytes), 0);
        for (unsigned i = 0; i < bytes; ++i) data[first + i] = static_cast<std::uint8_t>(value >> (8 * i));
        if (target && bytes == 4 && first < tags.size()) tags[first] = 1;
        return;
    }
    DspEngineRegion* engine = nullptr;
    if (port >= enginePointerPortBase) {
        const auto binding = port - enginePointerPortBase;
        if (binding >= references->engine.size()) return;
        engine = engineRegion(references->engine[binding], offset, bytes);
        if (!engine || !engine->writable) return;
        offset -= engine->start;
    }
    const green::Memory* destination = engine ? &engine->memory : &references->memory[port];
    const bool array = !engine && references->arrays[port].enabled;
    if (array) {
        const auto* region = arrayRegion(references->arrays[port], offset, bytes);
        if (!region || !region->writable) return;
        destination = &region->memory;
        offset -= region->start;
    }
    const auto& memory = *destination;
    if (offset < 0 || static_cast<std::uint64_t>(offset) > memory.size() ||
        memory.size() - static_cast<std::size_t>(offset) < bytes) return;
    const auto first = static_cast<std::size_t>(offset);
    // Invalidate overlapping targets, including views bound to another input of this node.
    const auto address = reinterpret_cast<std::uintptr_t>(memory.address()) + first;
    const auto invalidate = [&](const green::Memory& view, std::vector<std::uint8_t>& tags) {
        if (tags.empty()) return;
        const auto base = reinterpret_cast<std::uintptr_t>(view.address());
        if (address >= base + tags.size() || address + bytes <= base) return;
        const auto begin = address > base + 3 ? address - base - 3 : 0;
        const auto end = std::min<std::uintptr_t>(tags.size(), address + bytes - base);
        std::fill(tags.begin() + begin, tags.begin() + end, 0);
    };
    for (std::size_t input = 0; input < references->targets.size(); ++input) invalidate(references->memory[input], references->targets[input]);
    for (auto& binding : references->engine) {
        for (auto& region : binding.regions) invalidate(region.memory, region.targets);
        for (auto cell = engineCellsAt(binding, address); cell != binding.physicalCells.end() && binding.cells[*cell].address < address + bytes; ++cell)
            binding.cells[*cell].valid = false;
    }
    if (!array && target && bytes == 4) {
        auto& tags = engine ? engine->targets : references->targets[port];
        if (first < tags.size()) tags[first] = 1;
    }
    if (bytes == 1) memory.writeByte(static_cast<std::int32_t>(offset), static_cast<std::int32_t>(value & 0xffU));
    else memory.writeWordAt(first, lowBits(value));
    if (bytes == 8) memory.writeWordAt(first + 4, static_cast<std::uint32_t>(value >> 32));
}

std::uint64_t dspMemRefPresent(const DspMemoryReferences* references, std::uint64_t port) noexcept
{
    if (port == nativeFramePort) return 1;
    if (port >= enginePointerPortBase) {
        const auto binding = port - enginePointerPortBase;
        return binding < references->engine.size() && !references->engine[binding].regions.empty() && references->engine[binding].regions.front().memory.size() != 0;
    }
    return port < references->memory.size() && references->memory[port].size() != 0;
}

std::int64_t dspEngineOffset(const DspMemoryReferences* references, std::uint64_t port, std::int64_t base, std::int64_t offset, unsigned bytes) noexcept
{
    if (port < enginePointerPortBase || port == nativeFramePort) return offset;
    const auto binding = port - enginePointerPortBase;
    if (binding >= references->engine.size()) return -1;
    const auto& engine = references->engine[binding];
    const auto* region = engineRegion(engine, base, 0);
    return region && engineRegion(engine, offset, bytes) == region ? offset : -1;
}

std::uint64_t dspLoadEnginePointer(const DspMemoryReferences* references, std::uint64_t port, std::int64_t base, std::int64_t offset) noexcept
{
    if (port < enginePointerPortBase || port == nativeFramePort || port - enginePointerPortBase >= references->engine.size()) return 0;
    const auto& engine = references->engine[port - enginePointerPortBase];
    const auto* source = engineRegion(engine, offset, 1);
    if (!source) return 0;
    const auto address = reinterpret_cast<std::uintptr_t>(source->memory.address()) + static_cast<std::size_t>(offset - source->start);
    const auto found = engineCellsAt(engine, address);
    if (found == engine.physicalCells.end()) return 0;
    const auto& cell = engine.cells[*found];
    if (cell.address != address || !cell.valid || dspEngineOffset(references, port, base, offset, cell.bytes) < 0) return 0;
    // Host byte edits must not turn arbitrary integer data into a new capability.
    if (readMemory(engine.regions[cell.region].memory, cell.offset, cell.bytes) != cell.initialBits) return 0;
    return cell.target;
}

std::uint64_t dspIsMemRefTarget(const DspMemoryReferences* references, std::uint64_t port, std::int64_t offset) noexcept
{
    if (port != nativeFramePort && port >= enginePointerPortBase) {
        const auto binding = port - enginePointerPortBase;
        if (binding >= references->engine.size()) return 0;
        const auto* region = engineRegion(references->engine[binding], offset, 4);
        if (!region) return 0;
        offset -= region->start;
        return static_cast<std::uint64_t>(offset) < region->targets.size() && region->targets[static_cast<std::size_t>(offset)];
    }
    const auto& tags = port == nativeFramePort ? references->nativeFrameTargets : references->targets[port];
    return offset >= 0 && static_cast<std::uint64_t>(offset) < tags.size() && tags[static_cast<std::size_t>(offset)];
}

void dspReportDivideFault(DspMemoryReferences* references, std::uint64_t kind, std::uint64_t frame) noexcept
{
    if (references->fault.kind == DspFaultKind::None) references->fault = {static_cast<DspFaultKind>(kind), frame};
}

void runDspStage(std::span<const DspInstruction> code, std::span<DspLanes> slots,
                 std::span<const DspLanes> arrays, DspMemoryReferences& references,
                 std::span<DspValue> stack, std::uint64_t frame) noexcept
{
    std::array<std::size_t, dspNestingLimit> remaining;
    std::size_t depth = 0;
    std::size_t size = 0;
    std::size_t backwardBranches = 0;
    std::array<std::size_t, dspNestingLimit> returnAddresses;
    std::size_t callDepth = 0;
    std::size_t recursiveCalls = 0;
    for (std::size_t pc = 0; pc < code.size();) {
        const auto& op = code[pc++];
        switch (op.op) {
        case DspOp::Constant: {
            DspValue value{op.type};
            value.lanes.fill(op.bits);
            stack[size++] = value;
            break;
        }
        case DspOp::X87Load:
            dspX87Load(&slots[op.slot], stack[--size].lanes[0], static_cast<std::uint64_t>(op.type));
            break;
        case DspOp::X87Store: {
            DspValue value{op.type};
            value.lanes.fill(dspX87Store(&slots[op.slot], static_cast<std::uint64_t>(op.type)));
            stack[size++] = value;
            break;
        }
        case DspOp::X87Copy: slots[op.slot] = slots[op.count]; break;
        case DspOp::X87Exchange: std::swap(slots[op.slot], slots[op.count]); break;
        case DspOp::X87Unary: dspX87Unary(&slots[op.slot], op.bits); break;
        case DspOp::X87Binary: dspX87Binary(&slots[op.slot], &slots[op.count], op.bits); break;
        case DspOp::Load: stack[size++] = {op.type, selectLanes(slots[op.slot], op.lanes)}; break;
        case DspOp::LoadArrayPacket: stack[size++] = {op.type, selectLanes(arrays[op.slot], op.lanes)}; break;
        case DspOp::Store: store(slots, stack[--size], op); break;
        case DspOp::LoadIndexed: stack[size - 1] = loadIndexed(slots, stack[size - 1], op); break;
        case DspOp::LoadArrayInput: stack[size - 1] = loadIndexed(arrays, stack[size - 1], op); break;
        case DspOp::LoadAssemblyMemory: {
            const std::span<const DspLanes> memory = op.bits == static_cast<std::uint64_t>(AssemblyStorage::ArrayInput) ? arrays : slots;
            stack[size - 1] = loadAssemblyMemory(memory, stack[size - 1], op);
            break;
        }
        case DspOp::LoadAssemblyByte: {
            const auto offset = static_cast<std::int64_t>(signedInt(stack[size - 1].lanes[0]));
            const auto bytes = assemblyElementBytes(op.storageType);
            std::uint64_t value = 0;
            if (offset >= 0 && static_cast<std::uint64_t>(offset) < op.count * bytes) {
                const auto element = static_cast<std::size_t>(offset) / bytes;
                const auto storage = static_cast<AssemblyStorage>(op.bits);
                const auto source = storage == AssemblyStorage::ArrayInput ? arrays : std::span<const DspLanes>{slots};
                const auto raw = storage == AssemblyStorage::Packet ? source[op.slot][element] : source[op.slot + element][0];
                value = (raw >> ((static_cast<unsigned>(offset) % bytes) * 8)) & 0xffU;
            }
            stack[size - 1].type = DspType::Int;
            stack[size - 1].lanes.fill(value);
            break;
        }
        case DspOp::StoreAssemblyByte: {
            size -= 2;
            const auto offset = static_cast<std::int64_t>(signedInt(stack[size + 1].lanes[0]));
            const auto bytes = assemblyElementBytes(op.storageType);
            if (offset >= 0 && static_cast<std::uint64_t>(offset) < op.count * bytes) {
                const auto element = static_cast<std::size_t>(offset) / bytes;
                auto& raw = op.bits == static_cast<std::uint64_t>(AssemblyStorage::Packet)
                    ? slots[op.slot][element] : slots[op.slot + element][0];
                const auto shift = (static_cast<unsigned>(offset) % bytes) * 8;
                raw = (raw & ~(std::uint64_t{0xff} << shift)) | ((stack[size].lanes[0] & 0xffU) << shift);
            }
            break;
        }
        case DspOp::LoadAssemblyPacket: {
            const std::span<const DspLanes> memory = op.bits == static_cast<std::uint64_t>(AssemblyStorage::ArrayInput) ? arrays : slots;
            stack[size - 1] = loadAssemblyPacket(memory, stack[size - 1], op);
            break;
        }
        case DspOp::StoreAssemblyPacket:
            size -= 2;
            storeAssemblyPacket(slots, stack[size], stack[size + 1], op);
            break;
        case DspOp::Reinterpret128:
            reinterpret128(stack[size - 1], op.type);
            stack[size - 1].lanes = selectLanes(stack[size - 1].lanes, op.lanes);
            break;
        case DspOp::StoreAssemblyMemory:
            size -= 2;
            storeAssemblyMemory(slots, stack[size], stack[size + 1], op);
            break;
        case DspOp::LoadMemRef: case DspOp::LoadMemArrayField: case DspOp::LoadMemByte: {
            const auto offset = static_cast<std::int64_t>(signedInt(stack[size - 1].lanes[0]));
            const auto bytes = op.op == DspOp::LoadMemByte ? 1U : assemblyElementBytes(op.type);
            DspValue value{op.type};
            for (unsigned lane = 0; lane < assemblyPacketLanes(op.type); ++lane) {
                if (op.lanes.broadcast >= 0 ? lane != static_cast<unsigned>(op.lanes.broadcast) : !includes(op.lanes, lane)) continue;
                value.lanes[lane] = (op.op == DspOp::LoadMemArrayField ? dspReadMemArrayField : dspReadMemRef)(
                    &references, op.slot, dspEngineOffset(&references, op.slot, op.count ? signedInt(slots[op.count-1][0]) : engineRootAddress, offset + lane * bytes, bytes), bytes);
            }
            value.lanes = selectLanes(value.lanes, op.lanes);
            stack[size - 1] = value;
            break;
        }
        case DspOp::LoadEnginePointer: {
            const auto pointer = dspLoadEnginePointer(&references, op.slot, op.count ? signedInt(slots[op.count-1][0]) : engineRootAddress, signedInt(stack[size - 1].lanes[0]));
            stack[size - 1].type = DspType::Int;
            stack[size - 1].lanes.fill(pointer);
            break;
        }
        case DspOp::MemRefPresent: {
            auto& value = stack[size++];
            value.type = DspType::Int;
            value.lanes.fill(dspMemRefPresent(&references, op.slot));
            break;
        }
        case DspOp::IsMemRefTarget: {
            const auto valid = dspIsMemRefTarget(&references, op.slot, signedInt(stack[size - 1].lanes[0]));
            stack[size - 1].type = DspType::Bool;
            stack[size - 1].lanes.fill(valid ? UINT64_MAX : 0);
            break;
        }
        case DspOp::StoreMemRef: case DspOp::StoreMemByte: {
            size -= 2;
            const auto& value = stack[size];
            const auto offset = static_cast<std::int64_t>(signedInt(stack[size + 1].lanes[0]));
            const auto bytes = op.op == DspOp::StoreMemByte ? 1U : assemblyElementBytes(op.type);
            for (unsigned lane = 0; lane < assemblyPacketLanes(op.type); ++lane) {
                if (!includes(op.lanes, lane)) continue;
                const auto bits = dspConvert(value.lanes[lane], value.type, op.type);
                dspWriteMemRef(&references, op.slot, dspEngineOffset(&references, op.slot, op.count ? signedInt(slots[op.count-1][0]) : engineRootAddress, offset + lane * bytes, bytes), bytes, bits, static_cast<unsigned>(op.bits));
            }
            break;
        }
        case DspOp::StoreIndexed:
            size -= 2;
            store(slots, stack[size + 1], op, &stack[size]);
            break;
        case DspOp::Raw32:
            stack[size - 1].type = op.type;
            for (auto& lane : stack[size - 1].lanes) lane = lowBits(lane);
            break;
        case DspOp::SseMin: case DspOp::SseMax: case DspOp::SseCompare: case DspOp::ShufflePs: case DspOp::ShufflePd: case DspOp::SseAdd: case DspOp::SseSubtract: case DspOp::SseMultiply: case DspOp::SseDivide:
        case DspOp::MultiplyUnsignedDoublewords:
            --size;
            applySse(op, stack[size - 1], stack[size]);
            break;
        case DspOp::IntegerDivide: {
            size -= 3;
            const auto low = lowBits(stack[size].lanes[0]), high = lowBits(stack[size + 1].lanes[0]);
            const bool byte = op.bits == 8;
            const auto rawDivisor = lowBits(stack[size + 2].lanes[0]);
            const auto divisor = byte ? static_cast<std::int64_t>(static_cast<int>(rawDivisor & 0xffU) - ((rawDivisor & 0x80U) ? 256 : 0))
                : static_cast<std::int64_t>(std::bit_cast<std::int32_t>(rawDivisor));
            if (divisor == 0) {
                dspReportDivideFault(&references, static_cast<std::uint64_t>(DspFaultKind::IntegerDivideByZero), frame);
                return;
            }
            const auto bits = byte ? static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<int>(low & 0xffffU) - ((low & 0x8000U) ? 65536 : 0)))
                : (std::uint64_t{high} << 32) | low;
            const bool negative = byte ? (low & 0x8000U) != 0 : (high & 0x80000000U) != 0;
            const auto magnitude = negative ? std::uint64_t{0} - bits : bits;
            const auto divisorMagnitude = static_cast<std::uint64_t>(divisor < 0 ? -divisor : divisor);
            const bool quotientNegative = negative != (divisor < 0);
            const auto quotient = magnitude / divisorMagnitude, remainder = magnitude % divisorMagnitude;
            const auto limit = byte ? (quotientNegative ? 128ULL : 127ULL) : (quotientNegative ? 0x80000000ULL : 0x7fffffffULL);
            if (quotient > limit) {
                dspReportDivideFault(&references, static_cast<std::uint64_t>(DspFaultKind::IntegerDivideOverflow), frame);
                return;
            }
            DspValue q{DspType::Int}, rem{DspType::Int};
            q.lanes.fill(quotientNegative ? 0U - static_cast<std::uint32_t>(quotient) : static_cast<std::uint32_t>(quotient));
            rem.lanes.fill(negative ? 0U - static_cast<std::uint32_t>(remainder) : static_cast<std::uint32_t>(remainder));
            stack[size++] = q;
            stack[size++] = rem;
            break;
        }
        case DspOp::IntegerCarry: {
            --size;
            auto& left = stack[size - 1];
            const auto& right = stack[size];
            for (unsigned lane = 0; lane < left.lanes.size(); ++lane) {
                const auto a = lowBits(left.lanes[lane]), b = lowBits(right.lanes[lane]);
                left.lanes[lane] = mask(op.bits ? std::uint64_t{a} + b > UINT32_MAX : a < b);
            }
            left.type = DspType::Bool;
            break;
        }
        case DspOp::IntegerParity: {
            auto& value = stack[size - 1];
            for (auto& lane : value.lanes) lane = mask((std::popcount(lowBits(lane) & 0xffU) & 1) == 0);
            value.type = DspType::Bool;
            break;
        }
        case DspOp::Duplicate:
            stack[size] = stack[size - 1];
            ++size;
            break;
        case DspOp::PackedShiftCount: {
            auto& value = stack[size - 1];
            value.lanes.fill(lowBits(value.lanes[1]) == 0 ? std::min(lowBits(value.lanes[0]), 32U) : 32U);
            break;
        }
        case DspOp::Loop:
            if (op.count == 0) {
                pc = op.slot;
            } else {
                remaining[depth++] = op.count;
            }
            break;
        case DspOp::Repeat:
            if (--remaining[depth - 1] != 0) {
                pc = op.slot;
            } else {
                --depth;
            }
            break;
        case DspOp::Hop:
            if ((frame & (op.count - 1)) != 0) {
                pc = op.slot;
            }
            break;
        case DspOp::JumpIfTrue:
        case DspOp::Jump:
            if (op.op == DspOp::Jump || stack[--size].lanes.front() != 0) {
                if (op.slot < pc && backwardBranches++ == dspBackwardBranchLimit) return;
                pc = op.slot;
            }
            break;
        case DspOp::LocalCall:
            if (recursiveCalls++ == dspRecursiveCallLimit || callDepth == returnAddresses.size()) return;
            returnAddresses[callDepth++] = pc;
            pc = op.slot;
            break;
        case DspOp::LocalReturn:
            if (callDepth == 0) return;
            pc = returnAddresses[--callDepth];
            break;
        case DspOp::BindNativeFrame:
            slots[op.slot].fill(0);
            slots[op.count].fill(0);
            break;
        case DspOp::ReadSamplePosition:
            slots[op.slot].fill(dspSamplePosition(&references));
            break;
        case DspOp::ReadTimestamp: {
            const auto ticks = dspTimestamp();
            slots[op.slot].fill(lowBits(ticks));
            slots[op.count].fill(ticks >> 32);
            break;
        }
        case DspOp::Select:
            size -= 2;
            selectValue(stack[size - 1], stack[size], stack[size + 1], op.type);
            break;
        case DspOp::Negate: case DspOp::Complement: case DspOp::Sin1: case DspOp::Cos1: case DspOp::Tan1:
        case DspOp::Log10: case DspOp::Log2: case DspOp::Ln: case DspOp::Sign: case DspOp::Abs:
        case DspOp::Exp: case DspOp::Sqrt: case DspOp::RoundNearest: case DspOp::Round: case DspOp::Trunc:
        case DspOp::Floor: case DspOp::Ceil: case DspOp::Convert: case DspOp::Denorm: case DspOp::Exp2MinusOne:
        case DspOp::SseSqrt: case DspOp::SinRadians: case DspOp::CosRadians: case DspOp::TanRadians:
            applyUnary(op.op, op.type, stack[size - 1]);
            break;
        default:
            --size;
            applyBinary(op.op, op.type, stack[size - 1], stack[size]);
            break;
        }
    }
}

}
