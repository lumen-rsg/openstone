#include "Value.hpp"

#include <algorithm>
#include <charconv>
#include <ranges>

namespace openstone::yellow {
namespace {

using namespace std::string_view_literals;

// Each entry lists the aliases for the cursor at that numeric index.
constexpr std::array cursorNames{
    "default"sv,
    "ibeam beam"sv,
    "pointer arrow"sv,
    "handpointer"sv,
    "move"sv,
    "pointeradd"sv,
    "pointerdel"sv,
    "smallpointer"sv,
    "resizeew sizeew resizewe sizewe"sv,
    "resizens sizens resizesn sizesn"sv,
    "handopen"sv,
    "handclosed"sv,
    "crosshair"sv,
    "deny"sv,
    "hidden none null"sv,
    "resizenesw sizenesw resizeensw sizeensw resizenews sizenews resizeenws sizeenws "
    "resizeswen sizeswen resizewsne sizewsne resizewsen sizewsen"sv,
    "resizenwse sizenwse resizewnse sizewnse resizenwes sizenwes resizewnes sizewnes "
    "resizesenw sizesenw resizesewn sizesewn resizeesnw sizeesnw resizeeswn sizeeswn"sv,
    "resize size resizeall sizeall"sv,
    "splitew splitwe"sv,
    "splitns splitsn"sv,
    "help"sv,
    "wait"sv,
    "cross"sv,
    "hand"sv,
};

static_assert(cursorNames.size() == static_cast<std::size_t>(MouseCursor::Hand) + 1);

}

std::optional<MouseCursor> parseMouseCursor(std::string_view text)
{
    constexpr std::string_view whitespace = " \t\r\n\f\v";
    const auto first = text.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    text = text.substr(first, text.find_last_not_of(whitespace) - first + 1);
    const auto end = text.data() + text.size();
    unsigned int index = 0;
    if (const auto result = std::from_chars(text.data(), end, index);
        result.ec == std::errc{} && result.ptr == end && index < cursorNames.size()) {
        return static_cast<MouseCursor>(index);
    }

    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    for (std::size_t i = 0; i < cursorNames.size(); ++i) {
        for (const auto alias : cursorNames[i] | std::views::split(' ')) {
            if (std::string_view(alias.begin(), alias.end()) == lower) {
                return static_cast<MouseCursor>(i);
            }
        }
    }
    return std::nullopt;
}

}
