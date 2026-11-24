#pragma once

#include "green/Value.hpp"

#include <optional>
#include <stdexcept>

namespace openstone::array {

using green::Type;

inline constexpr std::size_t maxArrayPaddingBytes = 64 * 1024 * 1024;

inline bool isArray(Type type)
{
    return type == Type::IntArray || type == Type::FloatArray ||
           type == Type::StringArray || type == Type::PointArray;
}

inline std::optional<Type> arrayElement(Type type)
{
    switch (type) {
    case Type::IntArray: return Type::Int;
    case Type::FloatArray: return Type::Float;
    case Type::StringArray: return Type::String;
    case Type::PointArray: return Type::Point;
    default: return {};
    }
}

inline std::optional<Type> arrayForElement(Type type)
{
    switch (type) {
    case Type::Int: return Type::IntArray;
    case Type::Float: return Type::FloatArray;
    case Type::String: return Type::StringArray;
    case Type::Point: return Type::PointArray;
    default: return {};
    }
}

template<class Operation>
decltype(auto) withGreenArray(Type type, Operation operation)
{
    switch (type) {
    case Type::IntArray: return operation.template operator()<green::IntArray>();
    case Type::FloatArray: return operation.template operator()<green::FloatArray>();
    case Type::StringArray: return operation.template operator()<green::StringArray>();
    case Type::PointArray: return operation.template operator()<green::PointArray>();
    default: throw std::logic_error("Expected a Green array type");
    }
}

}
