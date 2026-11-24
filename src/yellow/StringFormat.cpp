#include "yellow/Value.hpp"

#include <algorithm>
#include <charconv>
#include <stdexcept>

namespace openstone::yellow {
namespace {

constexpr char asciiLower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string_view trim(std::string_view text)
{
    constexpr std::string_view whitespace = " \t\r\n\f\v";
    const auto start = text.find_first_not_of(whitespace);
    if (start == std::string_view::npos) {
        return {};
    }
    return text.substr(start, text.find_last_not_of(whitespace) - start + 1);
}

bool matches(std::string_view text, std::string_view name)
{
    return std::ranges::equal(text, name, {}, asciiLower);
}

}

StringFormat stringFormatFromFlags(std::uint32_t flags)
{
    if (flags & ~stringFormatFlagMask) throw std::invalid_argument("Unsupported String Format flags");
    StringFormat format;
    format.rightToLeft = flags & 1;
    format.verticalText = flags & 2;
    format.noFitBlackBox = flags & 4;
    format.showControls = flags & 32;
    format.noFontFallback = flags & 1024;
    format.includeTrailingSpaces = flags & 2048;
    format.wrap = !(flags & 4096);
    format.wholeLinesOnly = flags & 8192;
    format.clip = !(flags & 16384);
    return format;
}

std::uint32_t stringFormatFlags(const StringFormat& f) noexcept
{
    return (f.rightToLeft ? 1u : 0u) | (f.verticalText ? 2u : 0u) | (f.noFitBlackBox ? 4u : 0u) |
        (f.showControls ? 32u : 0u) | (f.noFontFallback ? 1024u : 0u) | (f.includeTrailingSpaces ? 2048u : 0u) |
        (f.wrap ? 0u : 4096u) | (f.wholeLinesOnly ? 8192u : 0u) | (f.clip ? 0u : 16384u);
}

StringFormat parseStringFormat(std::string_view style, std::string_view horizontal, std::string_view vertical)
{
    StringFormat format;
    style = trim(style);
    std::uint32_t numericFlags = 0;
    const auto parsed = std::from_chars(style.data(), style.data() + style.size(), numericFlags);
    const bool numeric = !style.empty() && parsed.ec == std::errc{} && parsed.ptr == style.data() + style.size();
    if (numeric) format = stringFormatFromFlags(numericFlags);
    horizontal = trim(horizontal);
    if (matches(horizontal, "center")) {
        format.horizontal = HorizontalAlignment::Center;
    } else if (matches(horizontal, "right")) {
        format.horizontal = HorizontalAlignment::Right;
    }
    vertical = trim(vertical);
    if (matches(vertical, "center")) {
        format.vertical = VerticalAlignment::Center;
    } else if (matches(vertical, "bottom")) {
        format.vertical = VerticalAlignment::Bottom;
    }

    std::string flags(style);
    std::ranges::transform(flags, flags.begin(), asciiLower);
    if (!numeric) {
        format.wrap = !flags.contains("nowrap");
        format.rightToLeft = flags.contains("righttoleft");
        format.verticalText = flags.contains("vertical");
        format.showControls = flags.contains("showcontrols");
        format.includeTrailingSpaces = flags.contains("trailingspaces");
        format.wholeLinesOnly = flags.contains("linelimit");
        format.clip = !flags.contains("noclip");
        format.noFitBlackBox = flags.contains("nofitblackbox");
        format.noFontFallback = flags.contains("nofontfallback");
    }
    return format;
}

std::string stringFormatStyleName(const StringFormat& format)
{
    std::string text;
    if (format.rightToLeft) {
        text += "righttoleft";
    }
    if (!format.wrap) {
        text += "nowrap";
    }
    if (format.verticalText) {
        text += "vertical";
    }
    if (format.showControls) text += "showcontrols";
    if (format.includeTrailingSpaces) text += "trailingspaces";
    if (format.wholeLinesOnly) text += "linelimit";
    if (!format.clip) text += "noclip";
    if (format.noFitBlackBox) text += "nofitblackbox";
    if (format.noFontFallback) text += "nofontfallback";
    return text.empty() ? "normal" : text;
}

std::string_view alignmentName(HorizontalAlignment alignment)
{
    switch (alignment) {
    case HorizontalAlignment::Left: return "left";
    case HorizontalAlignment::Center: return "center";
    case HorizontalAlignment::Right: return "right";
    }
    return "left";
}

std::string_view alignmentName(VerticalAlignment alignment)
{
    switch (alignment) {
    case VerticalAlignment::Top: return "top";
    case VerticalAlignment::Center: return "center";
    case VerticalAlignment::Bottom: return "bottom";
    }
    return "top";
}

}
