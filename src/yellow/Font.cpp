#include "Value.hpp"

#include <algorithm>

namespace openstone::yellow {

FontStyle parseFontStyle(std::string_view text)
{
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return {
        .bold = lower.contains("bold"),
        .italic = lower.contains("italic"),
        .underline = lower.contains("underline"),
        .strikeout = lower.contains("strike"),
    };
}

std::string fontStyleName(const FontStyle& style)
{
    std::string text;
    if (style.bold) {
        text += "bold";
    }
    if (style.italic) {
        text += "italic";
    }
    if (style.underline) {
        text += "underline";
    }
    if (style.strikeout) {
        text += "strike";
    }
    return text.empty() ? "normal" : text;
}

}
