#pragma once

#include "green/Value.hpp"
#include "math/SignalConversions.hpp"

#include <cstdint>

namespace openstone::green {

enum class ArithmeticOperation { Add, Subtract, Multiply, Divide, Modulus, Min, Max, Power, Logarithm, Atan2, Hypotenuse };
enum class UnaryOperation {
    Absolute, Round, Floor, Ceiling, Truncate, Ln, Log2, Log10,
    Sin, Cos, Tan, Asin, Acos, Atan, Sinh, Cosh, Tanh, Reciprocal, Sign, Inverse, FromDecibel, ToDecibel,
};

// Int results wrap to 32 bits. Division truncates toward zero.
// Zero divisors produce zero for Int and positive infinity for Float.
// Int powers with negative exponents return the truncated reciprocal, with zero for a zero base.
// Int logarithms truncate toward zero and return zero outside their domain.
[[nodiscard]] std::int32_t calculate(ArithmeticOperation operation, std::int32_t left, std::int32_t right);
[[nodiscard]] float calculate(ArithmeticOperation operation, float left, float right);

// Point and array operations apply per coordinate or element. Int magnitude wraps at INT32_MIN.
// Fixed-base Int logarithms truncate toward zero and return zero for nonpositive inputs.
[[nodiscard]] Value calculate(UnaryOperation operation, Value value);
[[nodiscard]] Value calculate(math::PitchConversion operation, Value value, float sampleRate);

}
