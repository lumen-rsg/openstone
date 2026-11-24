#include "StringConversions.hpp"
#include "Schematic.hpp"

#include <iconv.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <span>
#include <stdexcept>

namespace openstone {
namespace {

constexpr std::string_view whitespace = " \t\r\n\f\v";
constexpr std::string_view uppercaseHex = "0123456789ABCDEF";

std::string_view trim(std::string_view text)
{
    const auto start = text.find_first_not_of(whitespace);
    return start == std::string_view::npos ? std::string_view{}
        : text.substr(start, text.find_last_not_of(whitespace) - start + 1);
}

std::string latin1Character(std::int32_t code)
{
    const auto byte = static_cast<unsigned char>(code);
    if (byte < 0x80) return {static_cast<char>(byte)};
    return {static_cast<char>(0xC0 | (byte >> 6)), static_cast<char>(0x80 | (byte & 0x3F))};
}

std::int32_t latin1Code(std::string_view text)
{
    if (text.empty()) return 0;
    const auto lead = static_cast<unsigned char>(text.front());
    if (lead < 0x80) return lead;
    if (text.size() >= 2 && (lead == 0xC2 || lead == 0xC3)) {
        const auto next = static_cast<unsigned char>(text[1]);
        if ((next & 0xC0) == 0x80) return ((lead & 0x1F) << 6) | (next & 0x3F);
    }
    return 0;
}

std::string decodeBytes(std::string_view text, int base, int digitsPerByte)
{
    std::string result;
    result.reserve(text.size() / static_cast<std::size_t>(digitsPerByte));
    const auto invalid = [base] {
        return std::invalid_argument(base == 16 ? "Expected two hexadecimal digits per byte"
                                                : "Expected eight binary digits per byte");
    };
    int byte = 0;
    int digits = 0;
    for (const char character : text) {
        if (whitespace.contains(character)) continue;
        int digit{};
        const auto parsed = std::from_chars(&character, &character + 1, digit, base);
        if (parsed.ec != std::errc{}) throw invalid();
        byte = byte * base + digit;
        if (++digits == digitsPerByte) {
            result.push_back(static_cast<char>(byte));
            byte = 0;
            digits = 0;
        }
    }
    if (digits != 0) throw invalid();
    return result;
}

std::string encodeBinary(std::string_view bytes)
{
    std::string result;
    if (bytes.size() > result.max_size() / 8) throw std::length_error("Binary string is too long");
    result.reserve(bytes.size() * 8);
    for (const unsigned char byte : bytes) {
        for (int bit = 7; bit >= 0; --bit) {
            result.push_back((byte & (1 << bit)) ? '1' : '0');
        }
    }
    return result;
}

std::uint32_t parseUnsigned(std::string_view text, int base)
{
    text = trim(text);
    if (text.empty()) return 0;
    const bool negative = text.starts_with('-');
    if (negative || text.starts_with('+')) text.remove_prefix(1);
    if (base == 16 && (text.starts_with("0x") || text.starts_with("0X"))) text.remove_prefix(2);
    std::uint32_t value{};
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value, base);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        throw std::invalid_argument(base == 16 ? "Expected a 32-bit hexadecimal integer" : "Expected a 32-bit integer");
    }
    return negative ? std::uint32_t{0} - value : value;
}

std::string integerHex(std::int32_t value)
{
    char buffer[8];
    const auto end = std::to_chars(buffer, buffer + sizeof(buffer), static_cast<std::uint32_t>(value), 16).ptr;
    for (char& digit : std::span(buffer, end)) {
        if (digit >= 'a') digit = static_cast<char>(digit - ('a' - 'A'));
    }
    return {buffer, end};
}

std::string charsetName(std::string_view name)
{
    name = trim(name);
    if (name.empty()) return "UTF-8";
    int code{};
    const auto parsed = std::from_chars(name.data(), name.data() + name.size(), code);
    if (parsed.ec != std::errc{} || parsed.ptr != name.data() + name.size()) return std::string(name);
    if ((code >= 28591 && code <= 28599) || code == 28603 || code == 28605) {
        return "ISO-8859-" + std::to_string(code - 28590);
    }
    switch (code) {
    case 1200: return "UTF-16LE";
    case 1201: return "UTF-16BE";
    case 12000: return "UTF-32LE";
    case 12001: return "UTF-32BE";
    case 20127: return "ASCII";
    case 20866: return "KOI8-R";
    case 21866: return "KOI8-U";
    case 50220: return "ISO-2022-JP";
    case 51932: return "EUC-JP";
    case 54936: return "GB18030";
    case 65000: return "UTF-7";
    case 65001: return "UTF-8";
    default: return "CP" + std::to_string(code);
    }
}

template<class InputChar>
std::string transcode(iconv_t descriptor, std::string& source,
                      std::size_t (*convert)(iconv_t, InputChar**, std::size_t*, char**, std::size_t*))
{
    std::string result;
    InputChar* input = source.data();
    auto remaining = source.size();
    const auto append = [&](InputChar** next, std::size_t* size) {
        std::array<char, 4096> buffer;
        for (;;) {
            auto* output = buffer.data();
            auto capacity = buffer.size();
            const auto converted = convert(descriptor, next, size, &output, &capacity);
            const int error = errno;
            result.append(buffer.data(), buffer.size() - capacity);
            if (converted == 0) return;
            if (converted != static_cast<std::size_t>(-1) || error != E2BIG) {
                throw std::invalid_argument("Text cannot be converted between the selected charsets");
            }
        }
    };
    append(&input, &remaining);
    append(nullptr, nullptr); // Flush the final shift sequence for stateful encodings.
    return result;
}

std::string convertCharset(std::string source, std::string_view from, std::string_view to)
{
    const auto sourceName = charsetName(from);
    const auto targetName = charsetName(to);
    const auto descriptor = iconv_open(targetName.c_str(), sourceName.c_str());
    if (descriptor == reinterpret_cast<iconv_t>(-1)) {
        throw std::invalid_argument("Unsupported charset conversion from " + sourceName + " to " + targetName);
    }
    const struct Descriptor {
        iconv_t value;
        ~Descriptor() { iconv_close(value); }
    } converter{descriptor};
    return transcode(converter.value, source, &iconv);
}

char formatSpecifier(std::string_view pattern)
{
    constexpr std::string_view signedSpecifiers = "di";
    constexpr std::string_view floatSpecifiers = "faegFAEG";
    constexpr std::string_view alternateSpecifiers = "oxXfaegFAEG";
    constexpr std::string_view flags = "-+ #0";
    constexpr std::string_view digits = "0123456789";
    constexpr int maxFieldSize = 4096;
    char specifier = '\0';
    const auto invalid = [] {
        return std::invalid_argument("Expected one numeric format field, such as %04d or %.2f");
    };
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '%') continue;
        if (++i < pattern.size() && pattern[i] == '%') continue;
        if (specifier != '\0') throw invalid();
        const auto firstFlag = i;
        while (i < pattern.size() && flags.contains(pattern[i])) ++i;
        const auto usedFlags = pattern.substr(firstFlag, i - firstFlag);
        const auto readFieldSize = [&] {
            int size = 0;
            while (i < pattern.size() && digits.contains(pattern[i])) {
                size = 10 * size + (pattern[i++] - '0');
                if (size > maxFieldSize) throw std::length_error("Format width and precision are limited to 4096");
            }
        };
        readFieldSize();
        const bool precision = i < pattern.size() && pattern[i] == '.';
        if (precision) {
            ++i;
            readFieldSize();
        }
        const bool longModifier = i < pattern.size() && pattern[i] == 'l';
        if (longModifier) ++i;
        if (i == pattern.size()) throw invalid();

        specifier = pattern[i];
        const bool floating = floatSpecifiers.contains(specifier);
        if ((longModifier && !floating) ||
            (!floating && !std::string_view("diuoxXc").contains(specifier)) ||
            (usedFlags.contains('#') && !alternateSpecifiers.contains(specifier)) ||
            (usedFlags.find_first_of("+ ") != std::string_view::npos && !floating && !signedSpecifiers.contains(specifier)) ||
            (specifier == 'c' && (precision || usedFlags.contains('0')))) {
            throw invalid();
        }
    }
    return specifier;
}

std::string printNumber(const std::string& pattern, auto argument)
{
    const auto size = std::snprintf(nullptr, 0, pattern.c_str(), argument);
    if (size < 0) throw std::invalid_argument("Could not format the number");
    std::string result(static_cast<std::size_t>(size), '\0');
    const auto printed = std::snprintf(result.data(), result.size() + 1, pattern.c_str(), argument);
    if (printed != size) throw std::invalid_argument("Could not format the number");
    return result;
}

std::string formatNumber(const std::string& value, const std::string& pattern)
{
    if (pattern.empty()) return value;
    if (pattern.find('\0') != std::string::npos) throw std::invalid_argument("Format contains a null byte");
    using green::Type;
    using green::findValueConverter;
    const char specifier = formatSpecifier(pattern);
    if (specifier == '\0') return printNumber(pattern, 0);
    if (std::string_view("dic").contains(specifier)) {
        const auto number = findValueConverter(Type::String, Type::Int)(value);
        return printNumber(pattern, static_cast<int>(std::get<std::int32_t>(number)));
    }
    if (std::string_view("uoxX").contains(specifier)) {
        return printNumber(pattern, static_cast<unsigned>(parseUnsigned(value, 10)));
    }
    const auto number = findValueConverter(Type::String, Type::Float)(value);
    return printNumber(pattern, static_cast<double>(std::get<float>(number)));
}

}

std::string encodeHexBytes(std::string_view bytes)
{
    std::string result;
    if (bytes.size() > result.max_size() / 2) throw std::length_error("Hex string is too long");
    result.reserve(bytes.size() * 2);
    for (const unsigned char byte : bytes) {
        result.push_back(uppercaseHex[byte >> 4]);
        result.push_back(uppercaseHex[byte & 0xF]);
    }
    return result;
}

std::string decodeHexBytes(std::string_view text)
{
    return decodeBytes(text, 16, 2);
}

green::Value Schematic::readStringConversion(const SchematicNode& entry) const
{
    const auto text = [&](int input) { return std::get<std::string>(readInput(entry.id, input)); };
    const auto integer = [&] { return std::get<std::int32_t>(readInput(entry.id, 0)); };
    switch (entry.kind) {
    case PrimitiveKind::AsciiToString: return latin1Character(integer());
    case PrimitiveKind::StringToAscii: return latin1Code(text(0));
    case PrimitiveKind::StringToHex: return encodeHexBytes(text(0));
    case PrimitiveKind::HexToString: return decodeHexBytes(text(0));
    case PrimitiveKind::BinaryToHex: return encodeHexBytes(decodeBytes(text(0), 2, 8));
    case PrimitiveKind::HexToBinary: return encodeBinary(decodeHexBytes(text(0)));
    case PrimitiveKind::HexToInt: return std::bit_cast<std::int32_t>(parseUnsigned(text(0), 16));
    case PrimitiveKind::IntToHex: return integerHex(integer());
    case PrimitiveKind::StringCharset: return convertCharset(text(0), text(1), text(2));
    case PrimitiveKind::FormatString: return formatNumber(text(0), text(1));
    default: throw std::logic_error("Expected a String conversion primitive");
    }
}

}
