#include "green/ArrayOperations.hpp"

#include <algorithm>
#include <stdexcept>

namespace openstone::green {
namespace {

template<class Array, class Combine>
Array zipPadded(const Array& left, const Array& right, Combine combine)
{
    const typename Array::value_type missing{};
    const auto length = std::max(left.size(), right.size());
    Array result;
    result.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        result.push_back(combine(index < left.size() ? left[index] : missing,
                                 index < right.size() ? right[index] : missing));
    }
    return result;
}

template<class Result, class Array, class Scalar, class Combine>
Result broadcast(const Array& array, const Scalar& scalar, bool arrayOnLeft, Combine combine)
{
    Result result;
    result.reserve(array.size());
    for (const auto& element : array) {
        result.push_back(arrayOnLeft ? combine(element, scalar) : combine(scalar, element));
    }
    return result;
}

template<class Array, class Combine>
Array combineArray(const Array& array, const Value& other, bool arrayOnLeft, Combine combine)
{
    if (const auto* elements = std::get_if<Array>(&other)) {
        return arrayOnLeft ? zipPadded(array, *elements, combine) : zipPadded(*elements, array, combine);
    }
    if (const auto* scalar = std::get_if<typename Array::value_type>(&other)) {
        return broadcast<Array>(array, *scalar, arrayOnLeft, combine);
    }
    throw std::invalid_argument("Unsupported Green array operand types");
}

bool isArray(Type type)
{
    return type == Type::IntArray || type == Type::FloatArray || type == Type::StringArray ||
           type == Type::PointArray;
}

Value calculateArray(ArrayOperation operation, const Value& array, const Value& other, bool arrayOnLeft)
{
    const auto calculateNumber = [operation](auto left, auto right) {
        return calculate(operation, static_cast<float>(left), static_cast<float>(right));
    };
    switch (typeOf(array)) {
    case Type::StringArray:
        if (operation == ArrayOperation::Add) {
            return combineArray(std::get<StringArray>(array), other, arrayOnLeft,
                                [](const std::string& a, const std::string& b) { return a + b; });
        }
        break;
    case Type::IntArray: {
        const auto& elements = std::get<IntArray>(array);
        if (const auto* scalar = std::get_if<float>(&other)) {
            return broadcast<FloatArray>(elements, *scalar, arrayOnLeft, calculateNumber);
        }
        return combineArray(elements, other, arrayOnLeft, [operation](std::int32_t a, std::int32_t b) {
            return calculate(operation, a, b);
        });
    }
    case Type::FloatArray: {
        const auto& elements = std::get<FloatArray>(array);
        if (const auto* scalar = std::get_if<std::int32_t>(&other)) {
            return broadcast<FloatArray>(elements, *scalar, arrayOnLeft, calculateNumber);
        }
        return combineArray(elements, other, arrayOnLeft, calculateNumber);
    }
    case Type::PointArray:
        return combineArray(std::get<PointArray>(array), other, arrayOnLeft, [operation](Point a, Point b) {
            return Point{calculate(operation, a.x, b.x), calculate(operation, a.y, b.y)};
        });
    default:
        break;
    }
    throw std::invalid_argument("Unsupported Green array operation or operand types");
}

}

Value applyArrayOperation(ArrayOperation operation, const Value& left, const Value& right)
{
    if (isArray(typeOf(left))) {
        return calculateArray(operation, left, right, true);
    }
    return calculateArray(operation, right, left, false);
}

}
