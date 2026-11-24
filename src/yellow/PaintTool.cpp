#include "yellow/Value.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace openstone::yellow {

void validateRadialPath(const RadialPath& path)
{
    const auto count = path.points.size();
    if (count < 3 || count > maxRadialPathPoints || (!path.types.empty() && path.types.size() != count))
        throw std::invalid_argument("Invalid radial path point/type count");
    for (const auto p : path.points) if (!std::isfinite(p.x) || !std::isfinite(p.y) || std::abs(p.x) > 1000000 || std::abs(p.y) > 1000000)
        throw std::invalid_argument("Invalid radial path coordinate");
    if (path.types.empty()) return;
    bool needsStart = true;
    for (std::size_t i = 0; i < count; ++i) {
        const auto type = path.types[i];
        const auto base = type & 7;
        if ((type & ~0xb7) || (base != 0 && base != 1 && base != 3) || (needsStart && base != 0))
            throw std::invalid_argument("Invalid radial path point type");
        if (base == 0 && (type & 0x80)) throw std::invalid_argument("Invalid radial path start");
        if (base == 3) {
            if (i + 2 >= count || (type & 0x80) || (path.types[i+1] & 0x80) ||
                (path.types[i+1] & 7) != 3 || (path.types[i+2] & 7) != 3 ||
                (path.types[i+1] & ~0xb7) || (path.types[i+2] & ~0xb7))
                throw std::invalid_argument("Invalid radial path Bezier sequence");
            i += 2;
        }
        needsStart = path.types[i] & 0x80;
    }
}

Area radialPathBounds(const RadialPath& path)
{
    validateRadialPath(path);
    auto left = path.points[0].x, right = left, top = path.points[0].y, bottom = top;
    for (const auto p : path.points) {
        left = std::min(left, p.x); right = std::max(right, p.x);
        top = std::min(top, p.y); bottom = std::max(bottom, p.y);
    }
    if (left == right || top == bottom) throw std::invalid_argument("Invalid radial path bounds");
    return {left, top, right - left, bottom - top};
}

namespace {

std::string_view trim(std::string_view text)
{
    constexpr std::string_view whitespace = " \t\r\n\f\v";
    const auto start = text.find_first_not_of(whitespace);
    return start == std::string_view::npos ? std::string_view{}
        : text.substr(start, text.find_last_not_of(whitespace) - start + 1);
}

constexpr char asciiLower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool matches(std::string_view text, std::string_view name)
{
    return std::ranges::equal(text, name, {}, asciiLower);
}

constexpr std::array<std::string_view, 5> wraps{"tile", "flipx", "flipy", "flipxy", "clamp"};

// The order follows GDI+ HatchStyle IDs.
constexpr std::array<std::string_view, 53> hatches{
    "horizontal", "vertical", "forwarddiagonal", "backwarddiagonal", "cross", "diagonalcross",
    "05percent", "10percent", "20percent", "25percent", "30percent", "40percent", "50percent",
    "60percent", "70percent", "75percent", "80percent", "90percent",
    "lightdownwarddiagonal", "lightupwarddiagonal", "darkdownwarddiagonal", "darkupwarddiagonal",
    "widedownwarddiagonal", "wideupwarddiagonal", "lightvertical", "lighthorizontal",
    "narrowvertical", "narrowhorizontal", "darkvertical", "darkhorizontal", "dasheddownwarddiagonal",
    "dashedupwarddiagonal", "dashedhorizontal", "dashedvertical", "smallconfetti", "largeconfetti", "zigzag",
    "wave", "diagonalbrick", "horizontalbrick", "weave", "plaid", "divot", "dottedgrid", "dotteddiamond",
    "shingle", "trellis", "sphere", "smallgrid", "smallcheckerboard", "largecheckerboard",
    "outlineddiamond", "soliddiamond",
};

template <typename Option, std::size_t N>
Option parseOption(std::string_view text, const std::array<std::string_view, N>& names)
{
    if (text.empty()) {
        return {};
    }
    const auto found = std::ranges::find_if(names, [text](auto name) { return matches(text, name); });
    if (found != names.end()) {
        return static_cast<Option>(found - names.begin());
    }

    unsigned index = 0;
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, index);
    return result.ec == std::errc{} && result.ptr == end && index < names.size() ? static_cast<Option>(index) : Option{};
}

constexpr std::array styles{
    std::pair{LineStyle::Solid, std::string_view{"solid"}},
    std::pair{LineStyle::Dash, std::string_view{"dash"}},
    std::pair{LineStyle::Dot, std::string_view{"dot"}},
    std::pair{LineStyle::DashDot, std::string_view{"dashdot"}},
    std::pair{LineStyle::DashDotDot, std::string_view{"dashdotdot"}},
};

}

LineStyle parseLineStyle(std::string_view text)
{
    text = trim(text);
    const auto match = std::ranges::find_if(styles, [text](const auto& style) { return matches(text, style.second); });
    return match == styles.end() ? LineStyle::Solid : match->first;
}

std::string_view lineStyleName(LineStyle style)
{
    const auto match = std::ranges::find_if(styles, [style](const auto& entry) { return entry.first == style; });
    return match == styles.end() ? styles.front().second : match->second;
}

BrushWrap parseBrushWrap(std::string_view text)
{
    return parseOption<BrushWrap>(trim(text), wraps);
}

std::string_view brushWrapName(BrushWrap wrap)
{
    return wraps.at(static_cast<unsigned>(wrap));
}

RadialShape parseRadialShape(std::string_view text)
{
    text = trim(text);
    return matches(text, "rect") || matches(text, "rectangle") ? RadialShape::Rectangle : RadialShape::Ellipse;
}

HatchStyle parseHatchStyle(std::string_view text)
{
    text = trim(text);
    if (matches(text, "downwarddiagonal")) {
        return HatchStyle::ForwardDiagonal;
    }
    if (matches(text, "upwarddiagonal")) {
        return HatchStyle::BackwardDiagonal;
    }
    if (matches(text, "largegrid")) {
        return HatchStyle::Cross;
    }
    return parseOption<HatchStyle>(text, hatches);
}

std::string_view hatchStyleName(HatchStyle style)
{
    return hatches.at(static_cast<unsigned>(style));
}

}
