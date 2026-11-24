#include "green/Value.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <locale>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace openstone::green {
namespace {

static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
constexpr float maxFiniteFloat = 1.0e38F;
constexpr std::array valueTypes{
    Type::Float, Type::Int, Type::Boolean, Type::String, Type::FloatArray,
    Type::IntArray, Type::StringArray, Type::Point, Type::PointArray,
    Type::View, Type::Area, Type::MouseMessage, Type::Color, Type::ColorMatrix, Type::PaintTool,
    Type::Font, Type::StringFormat, Type::Bitmap, Type::BitmapArray, Type::Bus,
    Type::MIDI, Type::Notes, Type::Voices, Type::Preset, Type::Mem, Type::MemArray, Type::RubyValue,
};
static_assert(valueTypes.size() == std::variant_size_v<Value>);

std::string_view trim(std::string_view text)
{
    constexpr std::string_view whitespace = " \t\r\n\f\v";
    const auto start = text.find_first_not_of(whitespace);
    if (start == std::string_view::npos) {
        return {};
    }
    return text.substr(start, text.find_last_not_of(whitespace) - start + 1);
}

template<class Number>
Number parseNumber(std::string_view text, Number (*outOfRange)(std::string_view) = nullptr)
{
    text = trim(text);
    if (text.starts_with('+') && !text.substr(1).starts_with('-')) {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return 0;
    }

    Number number{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
    if (result.ec == std::errc::result_out_of_range) {
        if (outOfRange) {
            return outOfRange({text.data(), result.ptr});
        }
        throw std::out_of_range("Green numeric text is outside the target range");
    }
    return number;
}

float parseOutOfRangeFloat(std::string_view prefix)
{
    // Reparse only the numeric prefix. Stream extraction saturates on overflow,
    // which distinguishes very large exponents from values too small for a float.
    std::istringstream input{std::string(prefix)};
    input.imbue(std::locale::classic());
    double magnitude{};
    input >> magnitude;
    if (std::abs(magnitude) > maxFiniteFloat) {
        constexpr auto infinity = std::numeric_limits<float>::infinity();
        return magnitude < 0 ? -infinity : infinity;
    }
    throw std::out_of_range("Green numeric text is outside the target range");
}

template<class Number, class... Format>
std::string formatNumber(Number number, Format... format)
{
    // Large enough for a binary32 value in fixed notation with six fractional digits.
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), number, format...);
    if (result.ec != std::errc{}) {
        throw std::runtime_error("Could not format Green number");
    }
    return {buffer, result.ptr};
}

Value floatToInt(const Value& value)
{
    const double integer = std::trunc(std::get<float>(value));
    if (!std::isfinite(integer) || integer < std::numeric_limits<std::int32_t>::min() ||
        integer > std::numeric_limits<std::int32_t>::max()) {
        throw std::out_of_range("Green Float cannot be represented as Int");
    }
    return static_cast<std::int32_t>(integer);
}

Value stringToBoolean(const Value& value)
{
    constexpr std::array<std::string_view, 4> trueTokens{"true", "on", "yes", "y"};
    const std::string_view text = std::get<std::string>(value);
    const auto asciiLower = [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    return std::ranges::any_of(trueTokens, [text, asciiLower](std::string_view token) {
        return std::ranges::equal(text, token, {}, asciiLower);
    });
}

template<class Array, class Parse>
Value parseArrayLines(const Value& value, Parse parse)
{
    std::string_view text = std::get<std::string>(value);
    Array array;
    while (!text.empty()) {
        const auto newline = text.find('\n');
        const auto line = trim(text.substr(0, newline));
        if (!line.empty()) {
            array.push_back(parse(line));
        }
        if (newline == std::string_view::npos) {
            break;
        }
        text.remove_prefix(newline + 1);
    }
    return array;
}

template<class Array, class Format>
Value formatArrayLines(const Value& value, Format format)
{
    std::string text;
    const auto& array = std::get<Array>(value);
    for (std::size_t index = 0; index < array.size(); ++index) {
        if (index != 0) {
            text += '\n';
        }
        text += format(array[index]);
    }
    return text;
}

Point parsePoint(std::string_view line)
{
    const auto comma = line.find(',');
    if (comma == std::string_view::npos) {
        throw std::invalid_argument("Green Point Array text requires an x, y pair on each line");
    }
    return {parseNumber<float>(line.substr(0, comma)), parseNumber<float>(line.substr(comma + 1))};
}

std::string formatPoint(Point point)
{
    return formatNumber(point.x, std::chars_format::general, 6) + ", " +
           formatNumber(point.y, std::chars_format::general, 6);
}

Value stringToArea(const Value& value)
{
    std::string_view text = std::get<std::string>(value);
    std::array<float, 4> coordinates{};
    for (std::size_t index = 0; index < coordinates.size(); ++index) {
        const auto comma = text.find(',');
        const bool last = index + 1 == coordinates.size();
        if ((comma == std::string_view::npos) != last) {
            throw std::invalid_argument("Area text requires x, y, width, height");
        }
        auto field = trim(text.substr(0, comma));
        if (field.starts_with('+') && !field.substr(1).starts_with('-')) {
            field.remove_prefix(1);
        }
        if (field.empty()) {
            throw std::invalid_argument("Area coordinates must be finite numbers");
        }
        const auto end = field.data() + field.size();
        const auto result = std::from_chars(field.data(), end, coordinates[index]);
        if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(coordinates[index])) {
            throw std::invalid_argument("Area coordinates must be finite numbers");
        }
        if (!last) {
            text.remove_prefix(comma + 1);
        }
    }
    return yellow::Area{coordinates[0], coordinates[1], coordinates[2], coordinates[3]};
}

Value stringToStringFormat(const Value& value)
{
    const auto text = trim(std::get<std::string>(value));
    const auto first = text.find(',');
    const auto second = first == std::string_view::npos ? first : text.find(',', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos || text.find(',', second + 1) != std::string_view::npos)
        throw std::invalid_argument("String Format text requires flags, horizontal alignment, vertical alignment");
    return yellow::parseStringFormat(text.substr(0, first), text.substr(first + 1, second - first - 1), text.substr(second + 1));
}

struct Conversion {
    Type source;
    Type target;
    ValueConverter convert;
};

constexpr auto conversions = std::to_array<Conversion>({
    {Type::Float, Type::Int, floatToInt},
    {Type::Int, Type::Float, [](const Value& value) -> Value {
        return static_cast<float>(std::get<std::int32_t>(value));
    }},
    {Type::Int, Type::Boolean, [](const Value& value) -> Value {
        return std::get<std::int32_t>(value) != 0;
    }},
    {Type::Boolean, Type::Int, [](const Value& value) -> Value {
        return std::int32_t{std::get<bool>(value)};
    }},
    {Type::String, Type::Boolean, stringToBoolean},
    {Type::Boolean, Type::String, [](const Value& value) -> Value {
        return std::string(std::get<bool>(value) ? "True" : "False");
    }},
    {Type::String, Type::Float, [](const Value& value) -> Value {
        return normalizeFloat(parseNumber<float>(std::get<std::string>(value), parseOutOfRangeFloat));
    }},
    {Type::String, Type::Int, [](const Value& value) -> Value {
        return parseNumber<std::int32_t>(std::get<std::string>(value));
    }},
    {Type::Float, Type::String, [](const Value& value) -> Value {
        return formatNumber(std::get<float>(value));
    }},
    {Type::Int, Type::String, [](const Value& value) -> Value {
        return formatNumber(std::get<std::int32_t>(value));
    }},
    {Type::String, Type::FloatArray, [](const Value& value) {
        return parseArrayLines<FloatArray>(value, [](auto line) { return parseNumber<float>(line); });
    }},
    {Type::FloatArray, Type::String, [](const Value& value) {
        return formatArrayLines<FloatArray>(value, [](float number) {
            return formatNumber(number, std::chars_format::fixed, 6);
        });
    }},
    {Type::String, Type::IntArray, [](const Value& value) {
        return parseArrayLines<IntArray>(value, [](auto line) { return parseNumber<std::int32_t>(line); });
    }},
    {Type::IntArray, Type::String, [](const Value& value) {
        return formatArrayLines<IntArray>(value, [](std::int32_t number) { return formatNumber(number); });
    }},
    {Type::String, Type::StringArray, [](const Value& value) -> Value {
        StringArray lines;
        for (const auto part : std::get<std::string>(value) | std::views::split('\n')) {
            std::string_view line(part.begin(), part.end());
            if (line.ends_with('\r')) {
                line.remove_suffix(1);
            }
            lines.emplace_back(line);
        }
        return lines;
    }},
    {Type::StringArray, Type::String, [](const Value& value) {
        return formatArrayLines<StringArray>(value, [](const std::string& text) -> const std::string& {
            return text;
        });
    }},
    {Type::String, Type::PointArray, [](const Value& value) {
        return parseArrayLines<PointArray>(value, parsePoint);
    }},
    {Type::PointArray, Type::String, [](const Value& value) {
        return formatArrayLines<PointArray>(value, formatPoint);
    }},
    {Type::String, Type::Area, stringToArea},
    {Type::Color, Type::PaintTool, [](const Value& value) -> Value {
        return yellow::PaintTool{yellow::SolidBrush{std::get<yellow::Color>(value)}};
    }},
    {Type::String, Type::StringFormat, stringToStringFormat},
    {Type::StringFormat, Type::String, [](const Value& value) -> Value {
        const auto& format = std::get<yellow::StringFormat>(value);
        return std::to_string(yellow::stringFormatFlags(format)) + "," + std::string(yellow::alignmentName(format.horizontal)) +
            "," + std::string(yellow::alignmentName(format.vertical));
    }},
});

}

float normalizeFloat(float value) noexcept
{
    constexpr auto infinity = std::numeric_limits<float>::infinity();
    if (value > maxFiniteFloat) {
        return infinity;
    }
    if (value < -maxFiniteFloat) {
        return -infinity;
    }
    return value;
}

Type typeOf(const Value& value)
{
    return valueTypes.at(value.index());
}

bool isValueType(Type type)
{
    return std::ranges::find(valueTypes, type) != valueTypes.end();
}

std::int32_t addWrappedInts(std::int32_t left, std::int32_t right) noexcept
{
    // Add in a wider type; C++23 narrows the result modulo 2^32.
    return static_cast<std::int32_t>(static_cast<std::int64_t>(left) + right);
}

ValueConverter findValueConverter(Type source, Type target)
{
    const auto entry = std::ranges::find_if(conversions, [=](const Conversion& conversion) {
        return conversion.source == source && conversion.target == target;
    });
    return entry == conversions.end() ? nullptr : entry->convert;
}

}
