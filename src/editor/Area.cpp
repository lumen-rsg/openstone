#include "Schematic.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace openstone {
namespace {

using green::Point;
using yellow::Area;

Area normalize(Area area)
{
    if (area.width < 0) {
        area.x += area.width;
        area.width = -area.width;
    }
    if (area.height < 0) {
        area.y += area.height;
        area.height = -area.height;
    }
    return area;
}

float alignedOrigin(float origin, float referenceExtent, float sourceExtent, std::string_view name,
                    std::string_view farEdge)
{
    if (name == "far" || name == farEdge || name == "2") {
        return (origin + referenceExtent) - sourceExtent;
    }
    if (name == "center" || name == "centre" || name == "middle" || name == "1") {
        return origin + (referenceExtent - sourceExtent) / 2;
    }
    return origin;
}

std::string alignmentToken(std::string text)
{
    constexpr std::string_view whitespace = " \t\r\n\f\v";
    const auto first = text.find_first_not_of(whitespace);
    if (first == std::string::npos) {
        return {};
    }
    text = text.substr(first, text.find_last_not_of(whitespace) - first + 1);
    std::ranges::transform(text, text.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return text;
}

bool finite(Point point)
{
    return std::isfinite(point.x) && std::isfinite(point.y);
}

bool pointInPolygon(std::span<const Point> polygon, Point point)
{
    if (polygon.size() < 3 || !finite(point) || !std::ranges::all_of(polygon, finite)) {
        return false;
    }
    bool inside = false;
    auto previous = polygon.back();
    for (const auto current : polygon) {
        // Count crossings to the right with a half-open interval at each vertex.
        if ((current.y > point.y) != (previous.y > point.y)) {
            const auto crossing = (previous.x - current.x) * (point.y - current.y) /
                                  (previous.y - current.y) + current.x;
            if (point.x < crossing) {
                inside = !inside;
            }
        }
        previous = current;
    }
    return inside;
}

}

green::Value Schematic::readAreaGeometry(const SchematicNode& entry, int output) const
{
    const auto area = [&](int input) { return std::get<Area>(readInput(entry.id, input)); };
    const auto point = [&](int input) { return std::get<Point>(readInput(entry.id, input)); };
    const auto number = [&](int input) { return std::get<float>(readInput(entry.id, input)); };
    const auto connected = [&](int input) {
        return std::ranges::any_of(links_, [&](const auto& link) {
            return link.target == entry.id && link.input == input;
        });
    };

    switch (entry.kind) {
    case PrimitiveKind::Area:
        return Area{number(0), number(1), number(2), number(3)};
    case PrimitiveKind::AreaToFloat: {
        const auto value = area(0);
        return std::array{value.x, value.y, value.width, value.height}.at(output);
    }
    case PrimitiveKind::AreaAroundCenterPoint: {
        const auto center = point(0);
        const auto width = number(connected(1) ? 1 : 2);
        const auto height = number(connected(2) ? 2 : 1);
        return Area{center.x - width / 2, center.y - height / 2, width, height};
    }
    case PrimitiveKind::AreaFromPoints: {
        const auto first = point(0);
        const auto second = point(1);
        return normalize({first.x, first.y, second.x - first.x, second.y - first.y});
    }
    case PrimitiveKind::AreaCenterPoint: {
        const auto value = area(0);
        return Point{value.x + value.width / 2, value.y + value.height / 2};
    }
    case PrimitiveKind::AlignArea: {
        auto source = area(1);
        if (connected(0) && connected(1)) {
            const auto reference = area(0);
            const auto horizontal = alignmentToken(std::get<std::string>(readInput(entry.id, 2)));
            const auto vertical = alignmentToken(std::get<std::string>(readInput(entry.id, 3)));
            source.x = alignedOrigin(reference.x, reference.width, source.width, horizontal, "right");
            source.y = alignedOrigin(reference.y, reference.height, source.height, vertical, "bottom");
        }
        return source;
    }
    case PrimitiveKind::AreaToPoints: {
        const auto value = normalize(area(0));
        return output == 0 ? Point{value.x, value.y} : Point{value.x + value.width, value.y + value.height};
    }
    case PrimitiveKind::AreaUnion: {
        if (!connected(0)) {
            return area(1);
        }
        const auto first = area(0);
        if (!connected(1)) {
            return first;
        }
        const auto second = area(1);
        const auto x = std::min(first.x, second.x);
        const auto y = std::min(first.y, second.y);
        return Area{x, y, std::max(first.x + first.width, second.x + second.width) - x,
                          std::max(first.y + first.height, second.y + second.height) - y};
    }
    case PrimitiveKind::PointInsideArea: {
        if (!connected(0)) {
            return false;
        }
        const auto value = normalize(area(0));
        const auto sample = point(1);
        return sample.x >= value.x && sample.x <= value.x + value.width &&
               sample.y >= value.y && sample.y <= value.y + value.height;
    }
    case PrimitiveKind::PointInsidePolygon: {
        const auto polygon = std::get<green::PointArray>(readInput(entry.id, 0));
        return pointInPolygon(polygon, point(1));
    }
    default:
        throw std::logic_error("Invalid Area primitive");
    }
}

}
