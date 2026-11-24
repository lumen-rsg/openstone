#include "stream/DspProgramData.hpp"

#include "platform.h"
extern "C" {
#include "softfloat.h"
}

#include <bit>
#include <cmath>
#include <cstdlib>
#include <numbers>

namespace openstone::stream::detail {
namespace {

// Private software-FPU state is thread-local, and each helper leaves it as found.
struct FloatingScope {
    uint_fast8_t rounding = softfloat_roundingMode, precision = extF80_roundingPrecision;
    uint_fast8_t flags = softfloat_exceptionFlags, tininess = softfloat_detectTininess;
    FloatingScope() noexcept
    {
        softfloat_roundingMode = softfloat_round_near_even;
        extF80_roundingPrecision = 80;
        softfloat_exceptionFlags = 0;
        softfloat_detectTininess = softfloat_tininess_afterRounding;
    }
    ~FloatingScope()
    {
        softfloat_roundingMode = rounding;
        extF80_roundingPrecision = precision;
        softfloat_exceptionFlags = flags;
        softfloat_detectTininess = tininess;
    }
};

extFloat80_t extended(std::uint16_t signExp, std::uint64_t significand) noexcept
{
    extFloat80_t value;
    value.signExp = signExp;
    value.signif = significand;
    return value;
}

extFloat80_t read(const DspLanes& slot) noexcept { return extended(slot[1], slot[0]); }
void write(DspLanes& slot, extFloat80_t value) noexcept { slot = {value.signif, value.signExp, 0, 0}; }
bool isNaN(extFloat80_t value) noexcept
{
    return (value.signExp & 0x7fff) == 0x7fff && (value.signif & 0x7fffffffffffffffULL) != 0;
}

double asDouble(extFloat80_t value) noexcept { return std::bit_cast<double>(extF80_to_f64(value).v); }
extFloat80_t fromDouble(double value) noexcept { return f64_to_extF80({std::bit_cast<std::uint64_t>(value)}); }

extFloat80_t scale(extFloat80_t value, extFloat80_t amount) noexcept
{
    if (isNaN(value) || isNaN(amount)) return extF80_add(value, amount);
    if ((amount.signExp & 0x7fff) == 0x7fff) {
        return extF80_mul(value, (amount.signExp & 0x8000) ? extended(0, 0) : extended(0x7fff, 0x8000000000000000ULL));
    }
    if (!value.signif || (value.signExp & 0x7fff) == 0x7fff) return value;
    auto exponent = (amount.signExp & 0x7fff) >= 0x400f
        ? ((amount.signExp & 0x8000) ? -65536 : 65536)
        : static_cast<int>(extF80_to_i32(amount, softfloat_round_minMag, false));
    const auto power = [](int n) {
        return n >= -16382 ? extended(static_cast<std::uint16_t>(16383 + n), 0x8000000000000000ULL)
            : extended(0, std::uint64_t{1} << (n + 16445));
    };
    // Intermediate steps are exact unless the final result already overflows or
    // rounds to zero. In particular, do not round an intermediate subnormal twice.
    while (exponent > 16383) {
        value = extF80_mul(value, power(16383));
        exponent -= 16383;
    }
    while (exponent < -16445) {
        value = extF80_mul(value, power(-16382));
        exponent += 16382;
    }
    return extF80_mul(value, power(exponent));
}

}

void dspX87Load(DspLanes* destination, std::uint64_t bits, std::uint64_t type) noexcept
{
    FloatingScope scope;
    switch (static_cast<DspType>(type)) {
    case DspType::Float: write(*destination, f32_to_extF80({static_cast<std::uint32_t>(bits)})); break;
    case DspType::Double: write(*destination, f64_to_extF80({bits})); break;
    case DspType::Int: write(*destination, i32_to_extF80(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(bits)))); break;
    default: std::abort();
    }
}

std::uint64_t dspX87Store(const DspLanes* source, std::uint64_t type) noexcept
{
    FloatingScope scope;
    const auto value = read(*source);
    switch (static_cast<DspType>(type)) {
    case DspType::Float: return extF80_to_f32(value).v;
    case DspType::Double: return extF80_to_f64(value).v;
    case DspType::Int: return static_cast<std::uint32_t>(extF80_to_i32(value, softfloat_round_near_even, false));
    default: std::abort();
    }
}

void dspX87Unary(DspLanes* slot, std::uint64_t operation) noexcept
{
    FloatingScope scope;
    const auto op = static_cast<DspOp>(operation);
    const auto value = read(*slot);
    if (op == DspOp::RoundNearest) {
        write(*slot, extF80_roundToInt(value, softfloat_round_near_even, false));
        return;
    }
    // Transcendental instructions still use the existing binary64 libm model.
    const auto x = asDouble(value);
    double result;
    switch (op) {
    case DspOp::SinRadians: result = std::sin(x); break;
    case DspOp::CosRadians: result = std::cos(x); break;
    case DspOp::TanRadians: result = std::tan(x); break;
    case DspOp::Exp2MinusOne: result = std::expm1(x * std::numbers::ln2); break;
    default: std::abort();
    }
    write(*slot, fromDouble(result));
}

void dspX87Binary(DspLanes* destination, const DspLanes* source, std::uint64_t operation) noexcept
{
    FloatingScope scope;
    const auto a = read(*destination), b = read(*source);
    extFloat80_t value;
    switch (static_cast<DspOp>(operation)) {
    case DspOp::Add: value = extF80_add(a, b); break;
    case DspOp::Subtract: value = extF80_sub(a, b); break;
    case DspOp::Multiply: value = extF80_mul(a, b); break;
    case DspOp::Divide: value = extF80_div(a, b); break;
    case DspOp::ScalePowerOfTwo: value = scale(a, b); break;
    case DspOp::Remainder:
        value = extF80_rem(a, b);
        // IEEE remainder uses a nearest quotient; FPREM truncates it toward zero.
        if (value.signif && (value.signExp & 0x7fff) != 0x7fff && ((value.signExp ^ a.signExp) & 0x8000)) {
            auto adjustment = b;
            adjustment.signExp = (b.signExp & 0x7fff) | (a.signExp & 0x8000);
            value = extF80_add(value, adjustment);
        }
        break;
    case DspOp::Log2: value = fromDouble(asDouble(a) * std::log2(asDouble(b))); break;
    default: std::abort();
    }
    write(*destination, value);
}

}
