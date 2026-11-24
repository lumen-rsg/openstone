#include "Schematic.hpp"
#include "ArrayTypes.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>

namespace openstone {
namespace {

using green::StringArray;

// Advance one UTF-8 code point. Treat an invalid byte as one unit so it survives
// split and join even when the supplied string is not valid UTF-8.
std::size_t firstCharacterSize(std::string_view text)
{
    const auto lead = static_cast<unsigned char>(text.front());
    if (lead < 0xC2 || lead > 0xF4) {
        return 1;
    }
    const std::size_t width = lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    if (text.size() < width) {
        return 1;
    }
    const auto second = static_cast<unsigned char>(text[1]);
    if ((lead == 0xE0 && second < 0xA0) || (lead == 0xED && second >= 0xA0) ||
        (lead == 0xF0 && second < 0x90) || (lead == 0xF4 && second >= 0x90)) {
        return 1;
    }
    for (std::size_t i = 1; i < width; ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            return 1;
        }
    }
    return width;
}

std::size_t prefixBytes(std::string_view text, std::size_t characters)
{
    const auto size = text.size();
    while (characters > 0 && !text.empty()) {
        text.remove_prefix(firstCharacterSize(text));
        --characters;
    }
    return size - text.size();
}

std::size_t characterCount(std::string_view text)
{
    std::size_t count = 0;
    while (!text.empty()) {
        text.remove_prefix(firstCharacterSize(text));
        ++count;
    }
    return count;
}

std::size_t nonnegative(std::int32_t value)
{
    return static_cast<std::size_t>(std::max(value, std::int32_t{0}));
}

std::int32_t position(std::size_t index)
{
    return static_cast<std::int32_t>(std::min<std::size_t>(index, std::numeric_limits<std::int32_t>::max()));
}

char asciiLower(char character)
{
    return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A')) : character;
}

std::int32_t findText(std::string source, std::string needle, bool caseSensitive, bool reverse)
{
    if (!caseSensitive) {
        std::ranges::transform(source, source.begin(), asciiLower);
        std::ranges::transform(needle, needle.begin(), asciiLower);
    }
    const auto index = reverse ? source.rfind(needle) : source.find(needle);
    return index == std::string::npos ? -1 : position(characterCount(std::string_view(source).substr(0, index)));
}

std::int32_t findEntry(const StringArray& entries, std::string_view needle, bool caseSensitive, bool reverse)
{
    const auto matches = [&](const std::string& entry) {
        return caseSensitive ? entry == needle : std::ranges::equal(entry, needle, {}, asciiLower, asciiLower);
    };
    const auto find = [&](auto&& range) -> std::int32_t {
        const auto found = std::ranges::find_if(range, matches);
        return found == range.end() ? -1 : position(static_cast<std::size_t>(found - range.begin()));
    };
    // Array Find reports an offset from the end in reverse mode.
    return reverse ? find(entries | std::views::reverse) : find(entries);
}

std::string replaceText(std::string source, std::string_view needle, std::string_view replacement, bool once)
{
    if (needle.empty()) return source;

    std::string result;
    std::size_t start = 0;
    for (auto match = source.find(needle); match != std::string::npos; match = source.find(needle, start)) {
        result.append(source, start, match - start);
        result.append(replacement);
        start = match + needle.size();
        if (once) break;
    }
    if (start == 0) return source;
    result.append(source, start);
    return result;
}

std::string joinEntries(const StringArray& entries, std::string_view separator)
{
    std::string result;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) result.append(separator);
        result.append(entries[i]);
    }
    return result;
}

StringArray splitText(std::string_view source, std::string_view delimiter, std::size_t width)
{
    StringArray result;
    const auto appendField = [&](std::string_view field) {
        if (width > 0 && !field.empty()) {
            while (!field.empty()) {
                const auto bytes = prefixBytes(field, width);
                result.emplace_back(field.substr(0, bytes));
                field.remove_prefix(bytes);
            }
        } else {
            result.emplace_back(field);
        }
    };

    if (delimiter.empty()) {
        if (width == 0) width = 1;
        if (!source.empty()) appendField(source);
        return result;
    }
    for (auto next = source.find(delimiter); next != std::string_view::npos; next = source.find(delimiter)) {
        appendField(source.substr(0, next));
        source.remove_prefix(next + delimiter.size());
    }
    appendField(source);
    return result;
}

}

green::Value Schematic::readArrayLength(int id) const
{
    const auto type = inputType(id, 0);
    if (!type) return std::int32_t{0};
    const auto value = readInput(id, 0);
    if (const auto* text = std::get_if<std::string>(&value)) return position(characterCount(*text));
    return array::withGreenArray(*type, [&]<class Array> {
        return position(std::get<Array>(value).size());
    });
}

green::Value Schematic::readStringOperation(const SchematicNode& entry, int output) const
{
    const auto text = [&](int input) { return std::get<std::string>(readInput(entry.id, input)); };
    const auto integer = [&](int input) { return std::get<std::int32_t>(readInput(entry.id, input)); };
    const auto flag = [&](int input) { return std::get<bool>(readInput(entry.id, input)); };
    const auto strings = [&] { return std::get<StringArray>(readInput(entry.id, 0)); };

    switch (entry.kind) {
    case PrimitiveKind::StringFind:
        return findText(text(0), text(1), flag(2), flag(3));
    case PrimitiveKind::StringReplace:
        return replaceText(text(0), text(1), text(2), flag(3));
    case PrimitiveKind::StringSplit: {
        const auto source = text(0);
        const auto offset = prefixBytes(source, nonnegative(integer(1)));
        return output == 0 ? source.substr(0, offset) : source.substr(offset);
    }
    case PrimitiveKind::StringExtract: {
        const auto source = text(0);
        const auto offset = prefixBytes(source, nonnegative(integer(1)));
        const auto length = prefixBytes(std::string_view(source).substr(offset), nonnegative(integer(2)));
        return source.substr(offset, length);
    }
    case PrimitiveKind::StringOverride: {
        auto source = text(0);
        const auto replacement = text(2);
        const auto offset = prefixBytes(source, nonnegative(integer(1)));
        const auto length = prefixBytes(std::string_view(source).substr(offset), characterCount(replacement));
        source.replace(offset, length, replacement);
        return source;
    }
    case PrimitiveKind::StringArrayFind:
        return findEntry(strings(), text(1), flag(2), flag(3));
    case PrimitiveKind::StringArraySplit: {
        const auto source = strings();
        const auto offset = std::min(nonnegative(integer(1)), source.size());
        const auto split = source.begin() + static_cast<StringArray::difference_type>(offset);
        return output == 0 ? StringArray(source.begin(), split) : StringArray(split, source.end());
    }
    case PrimitiveKind::StringArrayToString:
        return joinEntries(strings(), text(1));
    case PrimitiveKind::StringToStringArray:
        return splitText(text(0), text(1), nonnegative(integer(2)));
    case PrimitiveKind::SpecialCharacters: {
        constexpr std::array<std::string_view, 4> characters{"\r\n", "\n", "\r", "\t"};
        const auto index = static_cast<std::size_t>(integer(0));
        return index < characters.size() ? std::string(characters[index]) : std::string{};
    }
    default:
        return readStringConversion(entry);
    }
}

green::Value Schematic::readSystemInfo(const SchematicNode& entry, int output) const
{
    const auto& root = rootGraph();
    if (entry.kind == PrimitiveKind::SystemFolders) {
        const auto& folder = root.systemInfo_.folder;
        if (!folder) return std::string{};
        constexpr std::int32_t documentsId = 5;
        constexpr std::int32_t appDataId = 26;
        if (output == 0) return folder(documentsId);
        if (output == 1) return folder(appDataId);
        return folder(std::get<std::int32_t>(readInput(entry.id, 0)));
    }

    if (!root.systemFontNames_) refreshSystemFonts();
    const auto& fonts = *root.systemFontNames_;
    if (output == 0) return fonts;
    if (output == 1) return joinEntries(fonts, "\n");
    return position(fonts.size());
}

void Schematic::refreshSystemFonts() const
{
    const auto& root = rootGraph();
    const auto& fonts = root.systemInfo_.fonts;
    root.systemFontNames_ = fonts ? fonts() : StringArray{};
}

}
