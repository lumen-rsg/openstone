#include "green/Arithmetic.hpp"
#include "math/Logarithm.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace openstone::green {
namespace {

std::int32_t integerPower(std::int32_t base, std::int32_t exponent) noexcept
{
    if (exponent < 0) {
        if (base == -1) {
            return exponent % 2 == 0 ? 1 : -1;
        }
        return base == 1 ? 1 : 0;
    }

    // Square in unsigned arithmetic and keep only the low 32 bits of each product.
    std::uint32_t result = 1;
    std::uint64_t factor = static_cast<std::uint32_t>(base);
    for (; exponent > 0; exponent /= 2) {
        if (exponent % 2 != 0) {
            result = static_cast<std::uint32_t>(result * factor);
        }
        factor = static_cast<std::uint32_t>(factor * factor);
    }
    return static_cast<std::int32_t>(result);
}

std::int32_t integerLogarithm(std::int32_t value, std::int32_t base) noexcept
{
    if (value <= 0 || base <= 1) {
        return 0;
    }

    // Count divisions to keep exact powers from rounding down in floating point.
    std::int32_t result = 0;
    while (value >= base) {
        value /= base;
        ++result;
    }
    return result;
}

}

std::int32_t calculate(ArithmeticOperation operation, std::int32_t left, std::int32_t right)
{
    // Widen before arithmetic, including INT32_MIN / -1, then wrap when narrowing.
    const auto wideLeft = static_cast<std::int64_t>(left);
    switch (operation) {
    case ArithmeticOperation::Add: return addWrappedInts(left, right);
    case ArithmeticOperation::Subtract: return static_cast<std::int32_t>(wideLeft - right);
    case ArithmeticOperation::Multiply: return static_cast<std::int32_t>(wideLeft * right);
    case ArithmeticOperation::Divide: return right == 0 ? 0 : static_cast<std::int32_t>(wideLeft / right);
    case ArithmeticOperation::Modulus: return right == 0 ? 0 : static_cast<std::int32_t>(wideLeft % right);
    case ArithmeticOperation::Min: return std::min(left, right);
    case ArithmeticOperation::Max: return std::max(left, right);
    case ArithmeticOperation::Power: return integerPower(left, right);
    case ArithmeticOperation::Logarithm: return integerLogarithm(left, right);
    case ArithmeticOperation::Atan2: throw std::invalid_argument("Atan2 requires Float operands");
    case ArithmeticOperation::Hypotenuse: throw std::invalid_argument("Hypotenuse requires Float operands");
    }
    throw std::invalid_argument("Unknown Green arithmetic operation");
}

float calculate(ArithmeticOperation operation, float left, float right)
{
    switch (operation) {
    case ArithmeticOperation::Add: return left + right;
    case ArithmeticOperation::Subtract: return left - right;
    case ArithmeticOperation::Multiply: return left * right;
    case ArithmeticOperation::Divide:
        return right == 0.0F ? std::numeric_limits<float>::infinity() : left / right;
    case ArithmeticOperation::Modulus:
        return right == 0.0F ? std::numeric_limits<float>::quiet_NaN() : std::fmod(left, right);
    case ArithmeticOperation::Min: return std::fmin(left, right);
    case ArithmeticOperation::Max: return std::fmax(left, right);
    case ArithmeticOperation::Power: return std::pow(left, right);
    case ArithmeticOperation::Logarithm: return math::logarithm(left, right);
    case ArithmeticOperation::Atan2: return std::atan2(left, right);
    case ArithmeticOperation::Hypotenuse: {
        const float leftSquared = left * left;
        const float rightSquared = right * right;
        return std::sqrt(leftSquared + rightSquared);
    }
    }
    throw std::invalid_argument("Unknown Green arithmetic operation");
}

namespace {

void applyUnary(UnaryOperation operation, std::int32_t& value)
{
    switch (operation) {
    case UnaryOperation::Absolute:
        if (value < 0) {
            value = calculate(ArithmeticOperation::Subtract, std::int32_t{0}, value);
        }
        break;
    case UnaryOperation::Ln:
        value = value > 0 ? static_cast<std::int32_t>(std::log(static_cast<double>(value))) : 0;
        break;
    case UnaryOperation::Log2: value = integerLogarithm(value, 2); break;
    case UnaryOperation::Log10: value = integerLogarithm(value, 10); break;
    case UnaryOperation::Sign: value = value < 0 ? -1 : 1; break;
    case UnaryOperation::Inverse: value = calculate(ArithmeticOperation::Subtract, std::int32_t{0}, value); break;
    case UnaryOperation::Round:
    case UnaryOperation::Floor:
    case UnaryOperation::Ceiling:
    case UnaryOperation::Truncate:
        break;
    default:
        throw std::invalid_argument("The operation requires a Float operand");
    }
}

void applyUnary(UnaryOperation operation, float& value)
{
    switch (operation) {
    case UnaryOperation::Absolute: value = std::fabs(value); break;
    case UnaryOperation::Round: value = std::round(value); break;
    case UnaryOperation::Floor: value = std::floor(value); break;
    case UnaryOperation::Ceiling: value = std::ceil(value); break;
    case UnaryOperation::Truncate: value = std::trunc(value); break;
    case UnaryOperation::Ln: value = std::log(value); break;
    case UnaryOperation::Log2: value = std::log2(value); break;
    case UnaryOperation::Log10: value = std::log10(value); break;
    case UnaryOperation::Sin: value = std::sin(value); break;
    case UnaryOperation::Cos: value = std::cos(value); break;
    case UnaryOperation::Tan: value = std::tan(value); break;
    case UnaryOperation::Asin: value = std::asin(value); break;
    case UnaryOperation::Acos: value = std::acos(value); break;
    case UnaryOperation::Atan: value = std::atan(value); break;
    case UnaryOperation::Sinh: value = normalizeFloat(std::sinh(value)); break;
    case UnaryOperation::Cosh: value = normalizeFloat(std::cosh(value)); break;
    case UnaryOperation::Tanh: value = std::tanh(value); break;
    case UnaryOperation::Reciprocal:
        // FlowStone Alpha returns FLT_MAX for both signed zeros.
        value = value == 0.0F ? std::numeric_limits<float>::max() : 1.0F / value;
        break;
    case UnaryOperation::Sign: value = value < 0.0F ? -1.0F : 1.0F; break;
    case UnaryOperation::Inverse: value = -value; break;
    case UnaryOperation::FromDecibel: value = std::pow(10.0F, value / 20.0F); break;
    case UnaryOperation::ToDecibel: value = 20.0F * std::log10(value); break;
    }
}

void applyUnary(UnaryOperation operation, Point& value)
{
    applyUnary(operation, value.x);
    applyUnary(operation, value.y);
}

template<class Element>
    requires requires(UnaryOperation operation, Element& value) { applyUnary(operation, value); }
void applyUnary(UnaryOperation operation, std::vector<Element>& values)
{
    for (auto& value : values) {
        applyUnary(operation, value);
    }
}

}

Value calculate(UnaryOperation operation, Value value)
{
    std::visit([operation](auto& operand) {
        if constexpr (requires { applyUnary(operation, operand); }) {
            applyUnary(operation, operand);
        } else {
            throw std::invalid_argument("The operation requires a numeric Green value");
        }
    }, value);
    return value;
}

Value calculate(math::PitchConversion operation, Value value, float sampleRate)
{
    if (auto* samples = std::get_if<FloatArray>(&value)) {
        for (auto& sample : *samples) sample = math::convertPitch(operation, sample, sampleRate);
        return value;
    }
    return math::convertPitch(operation, std::get<float>(value), sampleRate);
}

}
