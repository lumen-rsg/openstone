#pragma once

#include "green/Arithmetic.hpp"
#include "green/Value.hpp"

namespace openstone::green {

using ArrayOperation = ArithmeticOperation;

// Evaluate two arrays of the same type by index, padding missing entries with zero or an empty string.
// With one array, apply the scalar to every element in operand order. A Float operand in numeric
// array/scalar arithmetic produces a Float Array. Pass values after any connector conversions.
// Int division truncates toward zero and returns zero for a zero divisor.
// Floating-point division in array operations returns positive infinity for a zero divisor.
// Unsupported operations or operand types throw std::invalid_argument.
[[nodiscard]] Value applyArrayOperation(ArrayOperation operation, const Value& left, const Value& right);

}
